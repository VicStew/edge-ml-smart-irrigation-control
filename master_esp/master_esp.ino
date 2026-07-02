#include <WiFi.h>
#include <esp_now.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

#define TINY_GSM_MODEM_SIM800
#define SerialAT Serial1

#include <ArduinoJson.h>
#include <TinyGsmClient.h>
#include <PubSubClient.h>

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "hourly_rainfall_forecaster.h"
#include "hourly_rainfall_preprocessing.h"

/* ============================
   CONFIGURATION
============================ */

#define WIFI_CHANNEL 4
#define CONTROL_INTERVAL_MS 30000
#define RAIN_AMOUNT_BLOCK_THRESHOLD_MM 1.00f
#define MIN_IRRIGATION_DURATION_SEC 60
#define MAX_IRRIGATION_DURATION_SEC 300

#define MAX_SECTIONS 10
#define MAX_SENSOR_NODES 32
#define WEATHER_HISTORY_LENGTH 24
#define TENSOR_ARENA_SIZE (48 * 1024)

#define SIM800_RX_PIN 16
#define SIM800_TX_PIN 17
#define SIM800_BAUD 9600

#define THINGSBOARD_PORT 1883
#define MQTT_BUFFER_SIZE 768
#define TELEMETRY_QUEUE_LENGTH 8
#define TELEMETRY_PAYLOAD_SIZE 512
#define GSM_RECONNECT_INTERVAL_MS 30000
#define MQTT_RECONNECT_INTERVAL_MS 10000

// Useful if a GPS Module is integrated
// const char GSM_PIN[] = "";
// const char GPRS_APN[] = "internet";
// const char GPRS_USER[] = "";
// const char GPRS_PASS[] = "";

const char THINGSBOARD_SERVER[] = "mqtt.eu.thingsboard.cloud";
const char THINGSBOARD_TOKEN[] = "76uO5U0HGmm9inOyHs8w";
const char THINGSBOARD_CLIENT_ID[] = "qp6q17c71aeuosv98ygp";

const char TB_TELEMETRY_TOPIC[] = "v1/devices/me/telemetry";
const char TB_ATTRIBUTES_TOPIC[] = "v1/devices/me/attributes";
const char TB_ATTRIBUTES_RESPONSE_TOPIC[] = "v1/devices/me/attributes/response/+";

/* ============================
   MESSAGE TYPES
============================ */

typedef enum {
  MSG_WEATHER_DATA = 1,
  MSG_CONTROL_CMD = 2,
  MSG_STATUS = 3,
  MSG_ACK = 4
} msg_type_t;

/* ============================
   WEATHER PAYLOAD
============================ */

typedef struct {
  uint32_t sample_time;
  float temperature_2m;
  float relative_humidity_2m;
  float vapour_pressure_deficit_kpa;
  float soil_temperature_0_to_7cm;
  float soil_moisture_0_to_7cm;
  float et0_fao_evapotranspiration;
  float shortwave_radiation;
} weather_payload_t;

/* ============================
   CONTROL PAYLOAD
============================ */

typedef struct {
  bool irrigate;
  bool spray_pesticide;
  bool apply_fertilizer;
  uint16_t irrigation_duration_sec;
} control_payload_t;

/* ============================
   STATUS PAYLOAD
============================ */

typedef struct {
  bool valve_state;
  bool spray_state;
  bool fertilizer_state;
} status_payload_t;

/* ============================
   FARM PACKET
============================ */

typedef struct {
  uint8_t source_mac[6];
  uint8_t destination_mac[6];

  uint8_t section_id;
  uint8_t node_id;

  uint32_t sequence;
  msg_type_t type;

  union {
    weather_payload_t weather;
    control_payload_t control;
    status_payload_t status;
  };

} __attribute__((packed)) farm_packet_t;

static const size_t FARM_PACKET_HEADER_SIZE =
  offsetof(farm_packet_t, weather);

static const size_t FARM_PACKET_WEATHER_SIZE =
  FARM_PACKET_HEADER_SIZE +
  sizeof(weather_payload_t);

static const size_t FARM_PACKET_CONTROL_SIZE =
  FARM_PACKET_HEADER_SIZE +
  sizeof(control_payload_t);

static const size_t FARM_PACKET_STATUS_SIZE =
  FARM_PACKET_HEADER_SIZE +
  sizeof(status_payload_t);

/* ============================
   SECTION NODE TABLE
============================ */

typedef struct {
  uint8_t mac[6];
  bool active;
} section_node_t;

section_node_t sections[MAX_SECTIONS];

typedef struct {
  bool active;
  control_payload_t control;
} manual_control_t;

manual_control_t manual_controls[MAX_SECTIONS];

/* ============================
   WEATHER HISTORY
============================ */

typedef struct {
  bool valid;
  uint8_t section_id;
  weather_payload_t latest;
  weather_payload_t history[WEATHER_HISTORY_LENGTH];
  uint8_t next_index;
  uint8_t count;
} weather_cache_t;

weather_cache_t weather_cache[MAX_SENSOR_NODES];

typedef struct {
  bool valid;
  float precipitation_mm;
} rainfall_forecast_t;

rainfall_forecast_t latest_rainfall_forecast = {
  false,
  0
};

/* ============================
   THINGSBOARD STATE
============================ */

TinyGsm modem(SerialAT);
TinyGsmClient gsm_client(modem);
PubSubClient mqtt(gsm_client);

typedef struct {
  bool pending;
  char payload[TELEMETRY_PAYLOAD_SIZE];
} telemetry_message_t;

telemetry_message_t telemetry_queue[TELEMETRY_QUEUE_LENGTH];
uint8_t telemetry_queue_head = 0;
uint8_t telemetry_queue_tail = 0;
uint8_t telemetry_queue_count = 0;

bool modem_ready = false;
bool mqtt_subscribed = false;
uint32_t last_gsm_reconnect_attempt = 0;
uint32_t last_mqtt_reconnect_attempt = 0;
uint32_t shared_attribute_request_id = 1;

/* ============================
   AI MODEL STATE
============================ */

const tflite::Model *rainfall_model = nullptr;
tflite::MicroInterpreter *rainfall_interpreter = nullptr;
TfLiteTensor *rainfall_input = nullptr;
TfLiteTensor *rainfall_amount_output = nullptr;

tflite::MicroMutableOpResolver<1> rainfall_resolver;
alignas(16) uint8_t tensor_arena[TENSOR_ARENA_SIZE];
bool rainfall_model_ready = false;

/* ============================
   TIMERS
============================ */

unsigned long last_control_cycle = 0;
uint32_t sequence_counter = 0;

/* ============================
   HELPER FUNCTIONS
============================ */

void print_mac(const uint8_t *mac) {
  Serial.printf(
    "%02X:%02X:%02X:%02X:%02X:%02X\n",
    mac[0], mac[1], mac[2],
    mac[3], mac[4], mac[5]
  );
}

float clamp_float(
  float value,
  float low,
  float high
) {
  if (value < low) {
    return low;
  }

  if (value > high) {
    return high;
  }

  return value;
}

void add_section_peer(
  const uint8_t *mac
) {
  if (esp_now_is_peer_exist(mac)) {
    return;
  }

  esp_now_peer_info_t peerInfo = {};

  memcpy(peerInfo.peer_addr, mac, 6);
  peerInfo.channel = WIFI_CHANNEL;
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) == ESP_OK) {
    Serial.println("Section peer added");
  } else {
    Serial.println("Failed to add section peer");
  }
}

bool enqueue_telemetry(
  const char *payload
) {
  if (telemetry_queue_count >= TELEMETRY_QUEUE_LENGTH) {
    Serial.println("Telemetry queue full");
    return false;
  }

  telemetry_message_t *message =
    &telemetry_queue[telemetry_queue_tail];

  strncpy(
    message->payload,
    payload,
    TELEMETRY_PAYLOAD_SIZE - 1
  );
  message->payload[TELEMETRY_PAYLOAD_SIZE - 1] = '\0';
  message->pending = true;

  telemetry_queue_tail =
    (telemetry_queue_tail + 1) %
    TELEMETRY_QUEUE_LENGTH;
  telemetry_queue_count++;

  return true;
}

void queue_weather_telemetry(
  const farm_packet_t *pkt
) {
  StaticJsonDocument<TELEMETRY_PAYLOAD_SIZE> doc;

  doc["section_id"] = pkt->section_id;
  doc["node_id"] = pkt->node_id;
  doc["sample_time"] = pkt->weather.sample_time;
  doc["temperature_2m"] = pkt->weather.temperature_2m;
  doc["relative_humidity_2m"] = pkt->weather.relative_humidity_2m;
  doc["vapour_pressure_deficit_kpa"] =
    pkt->weather.vapour_pressure_deficit_kpa;
  doc["soil_temperature_0_to_7cm"] =
    pkt->weather.soil_temperature_0_to_7cm;
  doc["soil_moisture_0_to_7cm"] =
    pkt->weather.soil_moisture_0_to_7cm;
  doc["et0_fao_evapotranspiration"] =
    pkt->weather.et0_fao_evapotranspiration;
  doc["shortwave_radiation"] =
    pkt->weather.shortwave_radiation;

  char payload[TELEMETRY_PAYLOAD_SIZE];
  size_t length =
    serializeJson(doc, payload, sizeof(payload));

  if (length == 0 || length >= sizeof(payload)) {
    Serial.println("Weather telemetry payload too large");
    return;
  }

  if (enqueue_telemetry(payload)) {
    Serial.println("Weather telemetry queued");
  }
}

void queue_section_status_telemetry(
  const farm_packet_t *pkt
) {
  StaticJsonDocument<256> doc;

  doc["section_id"] = pkt->section_id;
  doc["valve_state"] = pkt->status.valve_state;
  doc["spray_state"] = pkt->status.spray_state;
  doc["fertilizer_state"] = pkt->status.fertilizer_state;

  char payload[256];
  size_t length =
    serializeJson(doc, payload, sizeof(payload));

  if (length == 0 || length >= sizeof(payload)) {
    Serial.println("Status telemetry payload too large");
    return;
  }

  enqueue_telemetry(payload);
}

void weather_to_features(
  const weather_payload_t &weather,
  float *features
) {
  features[0] = weather.temperature_2m;
  features[1] = weather.relative_humidity_2m;
  features[2] = weather.vapour_pressure_deficit_kpa;
  features[3] = weather.soil_temperature_0_to_7cm;
  features[4] = weather.soil_moisture_0_to_7cm;
  features[5] = weather.shortwave_radiation;
  features[6] = weather.et0_fao_evapotranspiration;
}

/* ============================
   AI MODEL INIT
============================ */

void init_rainfall_model() {
  rainfall_model =
    tflite::GetModel(
      hourly_rainfall_forecaster_tflite
    );

  if (
    rainfall_model->version() !=
    TFLITE_SCHEMA_VERSION
  ) {
    Serial.println("Rainfall model schema mismatch");
    return;
  }

  if (rainfall_resolver.AddFullyConnected() != kTfLiteOk) {
    Serial.println("Failed to add FullyConnected op");
    return;
  }

  static tflite::MicroInterpreter interpreter(
    rainfall_model,
    rainfall_resolver,
    tensor_arena,
    TENSOR_ARENA_SIZE
  );

  rainfall_interpreter = &interpreter;

  if (rainfall_interpreter->AllocateTensors() != kTfLiteOk) {
    Serial.println("Rainfall tensor allocation failed");
    return;
  }

  rainfall_input = rainfall_interpreter->input(0);
  rainfall_amount_output = rainfall_interpreter->output(0);

  if (
    rainfall_input->type != kTfLiteFloat32 ||
    rainfall_input->dims->size != 2 ||
    rainfall_input->dims->data[1] !=
      HOURLY_RAINFALL_FEATURE_COUNT
  ) {
    Serial.println("Rainfall model input shape mismatch");
    return;
  }

  if (rainfall_amount_output->type != kTfLiteFloat32) {
    Serial.println("Rainfall model output type mismatch");
    return;
  }

  rainfall_model_ready = true;
  Serial.println("Rainfall ML model ready");
}

bool update_rainfall_forecast(
  const weather_payload_t &weather
) {
  if (!rainfall_model_ready) {
    latest_rainfall_forecast.valid = false;
    return false;
  }

  float features[HOURLY_RAINFALL_FEATURE_COUNT];
  weather_to_features(weather, features);

  for (int i = 0; i < HOURLY_RAINFALL_FEATURE_COUNT; i++) {
    rainfall_input->data.f[i] =
      (
        features[i] -
        HOURLY_RAINFALL_X_MEAN[i]
      ) / HOURLY_RAINFALL_X_STD[i];
  }

  if (rainfall_interpreter->Invoke() != kTfLiteOk) {
    latest_rainfall_forecast.valid = false;
    Serial.println("Rainfall inference failed");
    return false;
  }

  float amount_scaled =
    rainfall_amount_output->data.f[0];

  float amount_log =
    amount_scaled *
    HOURLY_RAINFALL_Y_AMOUNT_STD +
    HOURLY_RAINFALL_Y_AMOUNT_MEAN;

  float rainfall_mm = expm1f(amount_log);

  latest_rainfall_forecast.precipitation_mm =
    rainfall_mm > 0.0f ? rainfall_mm : 0.0f;
  latest_rainfall_forecast.valid = true;

  Serial.printf(
    "ML precipitation forecast: %.2f mm\n",
    latest_rainfall_forecast.precipitation_mm
  );

  return true;
}

/* ============================
   AI DECISION ENGINE
============================ */

control_payload_t run_ai_model(
  const weather_payload_t &weather
) {
  control_payload_t control;

  control.irrigate = false;
  control.spray_pesticide = false;
  control.apply_fertilizer = false;
  control.irrigation_duration_sec = 0;

  update_rainfall_forecast(weather);

  float forecast_amount_mm =
    latest_rainfall_forecast.valid ?
    latest_rainfall_forecast.precipitation_mm :
    0.0f;

  bool rain_expected =
    forecast_amount_mm >=
    RAIN_AMOUNT_BLOCK_THRESHOLD_MM;

  if (
    !rain_expected &&
    weather.et0_fao_evapotranspiration >= 0.20f &&
    weather.shortwave_radiation >= 300.0f &&
    weather.relative_humidity_2m <= 70.0f
  ) {
    control.irrigate = true;

    float evap_pressure =
      clamp_float(
        weather.et0_fao_evapotranspiration,
        0.20f,
        0.60f
      );

    control.irrigation_duration_sec =
      MIN_IRRIGATION_DURATION_SEC +
      (uint16_t)(
        (evap_pressure - 0.20f) *
        (
          MAX_IRRIGATION_DURATION_SEC -
          MIN_IRRIGATION_DURATION_SEC
        ) / 0.40f
      );
  }

  return control;
}

/* ============================
   SEND CONTROL
============================ */

void send_control_packet(
  uint8_t *section_mac,
  uint8_t section_id,
  control_payload_t control
) {
  farm_packet_t pkt;

  WiFi.macAddress(pkt.source_mac);
  memcpy(pkt.destination_mac, section_mac, 6);

  pkt.section_id = section_id;
  pkt.node_id = 0;

  pkt.sequence = sequence_counter++;
  pkt.type = MSG_CONTROL_CMD;
  pkt.control = control;

  esp_now_send(
    section_mac,
    (uint8_t*)&pkt,
    sizeof(pkt)
  );

  Serial.printf(
    "Control sent to section %d\n",
    section_id
  );
}

bool json_has_key(
  JsonObjectConst object,
  const char *key
) {
  return !object[key].isNull();
}

bool json_bool_value(
  JsonVariantConst value,
  bool fallback
) {
  if (value.is<bool>()) {
    return value.as<bool>();
  }

  if (value.is<int>()) {
    return value.as<int>() != 0;
  }

  if (value.is<const char*>()) {
    const char *text = value.as<const char*>();

    return (
      strcmp(text, "true") == 0 ||
      strcmp(text, "1") == 0 ||
      strcmp(text, "on") == 0 ||
      strcmp(text, "ON") == 0
    );
  }

  return fallback;
}

bool json_bool_key(
  JsonObjectConst object,
  const char *key,
  bool fallback
) {
  if (!json_has_key(object, key)) {
    return fallback;
  }

  return json_bool_value(object[key], fallback);
}

int json_int_key(
  JsonObjectConst object,
  const char *key,
  int fallback
) {
  if (!json_has_key(object, key)) {
    return fallback;
  }

  return object[key].as<int>();
}

bool extract_manual_control(
  JsonObjectConst object,
  uint8_t *section_id,
  bool *manual_enabled,
  control_payload_t *control
) {
  int section =
    json_int_key(
      object,
      "section_id",
      json_int_key(
        object,
        "target_section_id",
        json_int_key(object, "section", -1)
      )
    );

  if (section < 0 || section >= MAX_SECTIONS) {
    Serial.println("Manual control missing valid section_id");
    return false;
  }

  *section_id = (uint8_t)section;
  *manual_enabled =
    json_bool_key(
      object,
      "enabled",
      json_bool_key(object, "manual", true)
    );

  control->irrigate =
    json_bool_key(
      object,
      "irrigate",
      json_bool_key(object, "valve", false)
    );
  control->spray_pesticide =
    json_bool_key(object, "spray_pesticide", false);
  control->apply_fertilizer =
    json_bool_key(object, "apply_fertilizer", false);

  int duration =
    json_int_key(
      object,
      "irrigation_duration_sec",
      json_int_key(object, "duration_sec", 0)
    );

  control->irrigation_duration_sec =
    duration > 0 ? (uint16_t)duration : 0;

  return true;
}

void publish_manual_control_result(
  uint8_t section_id,
  bool manual_enabled,
  bool forwarded
) {
  StaticJsonDocument<192> doc;

  doc["manual_control_section_id"] = section_id;
  doc["manual_control_enabled"] = manual_enabled;
  doc["manual_control_forwarded"] = forwarded;

  char payload[192];
  size_t length =
    serializeJson(doc, payload, sizeof(payload));

  if (length > 0 && length < sizeof(payload)) {
    enqueue_telemetry(payload);
  }
}

bool forward_manual_control(
  uint8_t section_id,
  bool manual_enabled,
  control_payload_t control
) {
  if (section_id >= MAX_SECTIONS) {
    return false;
  }

  if (manual_enabled) {
    manual_controls[section_id].active = true;
    manual_controls[section_id].control = control;
  } else {
    manual_controls[section_id].active = false;

    control.irrigate = false;
    control.spray_pesticide = false;
    control.apply_fertilizer = false;
    control.irrigation_duration_sec = 0;
  }

  if (!sections[section_id].active) {
    Serial.printf(
      "Section %d is not discovered yet\n",
      section_id
    );
    return false;
  }

  send_control_packet(
    sections[section_id].mac,
    section_id,
    control
  );

  return true;
}

void process_manual_control_object(
  JsonObjectConst object
) {
  uint8_t section_id = 0;
  bool manual_enabled = true;
  control_payload_t control = {};

  if (
    !extract_manual_control(
      object,
      &section_id,
      &manual_enabled,
      &control
    )
  ) {
    return;
  }

  bool forwarded =
    forward_manual_control(
      section_id,
      manual_enabled,
      control
    );

  publish_manual_control_result(
    section_id,
    manual_enabled,
    forwarded
  );

  Serial.printf(
    "Manual control section %d %s\n",
    section_id,
    forwarded ? "forwarded" : "queued as override"
  );
}

void process_shared_attributes(
  JsonObjectConst attributes
) {
  JsonVariantConst snake_case_control =
    attributes["manual_control"];
  JsonVariantConst camel_case_control =
    attributes["manualControl"];

  if (snake_case_control.is<JsonObjectConst>()) {
    process_manual_control_object(
      snake_case_control.as<JsonObjectConst>()
    );
    return;
  }

  if (camel_case_control.is<JsonObjectConst>()) {
    process_manual_control_object(
      camel_case_control.as<JsonObjectConst>()
    );
    return;
  }

  if (
    json_has_key(attributes, "section_id") ||
    json_has_key(attributes, "target_section_id") ||
    json_has_key(attributes, "section")
  ) {
    process_manual_control_object(attributes);
  }
}

void mqtt_callback(
  char *topic,
  byte *payload,
  unsigned int length
) {
  Serial.print("ThingsBoard message [");
  Serial.print(topic);
  Serial.println("]");

  StaticJsonDocument<MQTT_BUFFER_SIZE> doc;
  DeserializationError error =
    deserializeJson(doc, payload, length);

  if (error) {
    Serial.println("Failed to parse ThingsBoard attributes");
    return;
  }

  JsonObjectConst root = doc.as<JsonObjectConst>();

  if (root["shared"].is<JsonObjectConst>()) {
    process_shared_attributes(
      root["shared"].as<JsonObjectConst>()
    );
    return;
  }

  process_shared_attributes(root);
}

bool connect_gprs() {
  if (GSM_PIN[0] != '\0' && modem.getSimStatus() != 3) {
    modem.simUnlock(GSM_PIN);
  }

  if (!modem.isNetworkConnected()) {
    Serial.println("Waiting for GSM network...");

    if (!modem.waitForNetwork(60000L, true)) {
      Serial.println("GSM network unavailable");
      return false;
    }
  }

  if (!modem.isGprsConnected()) {
    Serial.print("Connecting GPRS APN ");
    Serial.println(GPRS_APN);

    if (!modem.gprsConnect(GPRS_APN, GPRS_USER, GPRS_PASS)) {
      Serial.println("GPRS connect failed");
      return false;
    }
  }

  return true;
}

void request_shared_attributes() {
  char topic[64];
  snprintf(
    topic,
    sizeof(topic),
    "v1/devices/me/attributes/request/%lu",
    (unsigned long)shared_attribute_request_id++
  );

  const char request[] =
    "{\"sharedKeys\":\"manual_control,manualControl,"
    "section_id,target_section_id,section,enabled,manual,"
    "irrigate,valve,spray_pesticide,apply_fertilizer,"
    "irrigation_duration_sec,duration_sec\"}";

  mqtt.publish(topic, request);
}

bool connect_thingsboard() {
  if (!connect_gprs()) {
    return false;
  }

  Serial.print("Connecting ThingsBoard MQTT: ");
  Serial.println(THINGSBOARD_SERVER);

  bool connected =
    mqtt.connect(
      THINGSBOARD_CLIENT_ID,
      THINGSBOARD_TOKEN,
      ""
    );

  if (!connected) {
    Serial.printf(
      "ThingsBoard MQTT failed, state=%d\n",
      mqtt.state()
    );
    mqtt_subscribed = false;
    return false;
  }

  mqtt.subscribe(TB_ATTRIBUTES_TOPIC);
  mqtt.subscribe(TB_ATTRIBUTES_RESPONSE_TOPIC);
  request_shared_attributes();

  mqtt_subscribed = true;
  Serial.println("ThingsBoard MQTT connected");
  return true;
}

void init_gsm_modem() {
  SerialAT.begin(
    SIM800_BAUD,
    SERIAL_8N1,
    SIM800_RX_PIN,
    SIM800_TX_PIN
  );

  delay(3000);

  Serial.println("Initializing SIM800L modem...");

  if (!modem.restart()) {
    Serial.println("SIM800L restart failed, trying init");

    if (!modem.init()) {
      Serial.println("SIM800L init failed");
      modem_ready = false;
      return;
    }
  }

  Serial.print("Modem: ");
  Serial.println(modem.getModemInfo());

  modem_ready = true;
}

void maintain_thingsboard() {
  if (!modem_ready) {
    unsigned long now = millis();

    if (
      now - last_gsm_reconnect_attempt >=
      GSM_RECONNECT_INTERVAL_MS
    ) {
      last_gsm_reconnect_attempt = now;
      init_gsm_modem();
    }

    return;
  }

  if (mqtt.connected()) {
    mqtt.loop();
    return;
  }

  unsigned long now = millis();

  if (
    now - last_mqtt_reconnect_attempt <
    MQTT_RECONNECT_INTERVAL_MS
  ) {
    return;
  }

  last_mqtt_reconnect_attempt = now;
  connect_thingsboard();
}

void publish_queued_telemetry() {
  if (!mqtt.connected()) {
    return;
  }

  while (telemetry_queue_count > 0) {
    telemetry_message_t *message =
      &telemetry_queue[telemetry_queue_head];

    if (
      !mqtt.publish(
        TB_TELEMETRY_TOPIC,
        message->payload
      )
    ) {
      Serial.println("Telemetry publish failed");
      return;
    }

    message->pending = false;
    telemetry_queue_head =
      (telemetry_queue_head + 1) %
      TELEMETRY_QUEUE_LENGTH;
    telemetry_queue_count--;

    Serial.println("Telemetry published to ThingsBoard");
  }
}

/* ============================
   HANDLE WEATHER PACKET
============================ */

void handle_weather_packet(
  farm_packet_t *pkt,
  const uint8_t *section_mac
) {
  if (
    pkt->node_id >= MAX_SENSOR_NODES ||
    pkt->section_id >= MAX_SECTIONS
  ) {
    Serial.println("Weather packet index out of range");
    return;
  }

  memcpy(
    sections[pkt->section_id].mac,
    section_mac,
    6
  );

  sections[pkt->section_id].active = true;
  add_section_peer(section_mac);

  weather_cache_t *cache =
    &weather_cache[pkt->node_id];

  cache->valid = true;
  cache->section_id = pkt->section_id;
  cache->latest = pkt->weather;
  cache->history[cache->next_index] = pkt->weather;
  cache->next_index =
    (cache->next_index + 1) %
    WEATHER_HISTORY_LENGTH;

  if (cache->count < WEATHER_HISTORY_LENGTH) {
    cache->count++;
  }

  Serial.printf(
    "Weather packet from Section %d Node %d stored (%d/%d samples)\n",
    pkt->section_id,
    pkt->node_id,
    cache->count,
    WEATHER_HISTORY_LENGTH
  );
  Serial.printf("temperature_2m: %.2f\n", pkt->weather.temperature_2m);
  Serial.printf("relative_humidity_2m: %.2f\n", pkt->weather.relative_humidity_2m);
  Serial.printf("vapour_pressure_deficit_kpa: %.3f\n", pkt->weather.vapour_pressure_deficit_kpa);
  Serial.printf("soil_temperature_0_to_7cm: %.2f\n", pkt->weather.soil_temperature_0_to_7cm);
  Serial.printf("soil_moisture_0_to_7cm: %.3f\n", pkt->weather.soil_moisture_0_to_7cm);
  Serial.printf("et0_fao_evapotranspiration: %.3f\n", pkt->weather.et0_fao_evapotranspiration);
  Serial.printf("shortwave_radiation: %.2f\n", pkt->weather.shortwave_radiation);

  queue_weather_telemetry(pkt);
}

/* ============================
   HANDLE STATUS PACKET
============================ */

void handle_status_packet(
  farm_packet_t *pkt
) {
  Serial.printf("Section %d status:\n", pkt->section_id);
  Serial.printf("Valve: %s\n", pkt->status.valve_state ? "ON" : "OFF");
  Serial.printf("Spray: %s\n", pkt->status.spray_state ? "ON" : "OFF");
  Serial.printf("Fertilizer: %s\n", pkt->status.fertilizer_state ? "ON" : "OFF");

  queue_section_status_telemetry(pkt);
}

bool packet_size_is_valid(
  msg_type_t type,
  size_t len
) {
  if (len == sizeof(farm_packet_t)) {
    return true;
  }

  switch (type) {
    case MSG_WEATHER_DATA:
      return len == FARM_PACKET_WEATHER_SIZE;

    case MSG_CONTROL_CMD:
      return len == FARM_PACKET_CONTROL_SIZE;

    case MSG_STATUS:
      return len == FARM_PACKET_STATUS_SIZE;

    default:
      return false;
  }
}

/* ============================
   RECEIVE CALLBACK
============================ */

void on_data_recv(
  const esp_now_recv_info_t *recv_info,
  const uint8_t *incoming_data,
  int len
) {
  if (len < (int)FARM_PACKET_HEADER_SIZE) {
    Serial.printf(
      "Invalid packet size: %d bytes, expected at least %u\n",
      len,
      (unsigned int)FARM_PACKET_HEADER_SIZE
    );
    return;
  }

  farm_packet_t pkt = {};
  size_t copy_len =
    len < (int)sizeof(pkt) ?
    (size_t)len :
    sizeof(pkt);

  memcpy(&pkt, incoming_data, copy_len);

  if (!packet_size_is_valid(pkt.type, (size_t)len)) {
    Serial.printf(
      "Invalid packet size: %d bytes for type %d\n",
      len,
      (int)pkt.type
    );
    return;
  }

  switch (pkt.type) {
    case MSG_WEATHER_DATA:
      handle_weather_packet(
        &pkt,
        recv_info->src_addr
      );
      break;

    case MSG_STATUS:
      handle_status_packet(&pkt);
      break;

    default:
      Serial.println("Unknown packet type");
      break;
  }
}

/* ============================
   SEND CALLBACK
============================ */

void on_data_sent(
  const wifi_tx_info_t *tx_info,
  esp_now_send_status_t status
) {
  Serial.print("Send Status: ");

  if (status == ESP_NOW_SEND_SUCCESS) {
    Serial.println("Success");
  } else {
    Serial.println("Failed");
  }

  if (tx_info) {
    Serial.print("Address received the packet: ");
    print_mac(tx_info->des_addr);
  }
}

/* ============================
   INIT ESPNOW
============================ */

void init_espnow() {
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    ESP.restart();
  }

  esp_now_register_recv_cb(on_data_recv);
  esp_now_register_send_cb(on_data_sent);
}

/* ============================
   CONTROL LOOP
============================ */

void process_control_cycle() {
  for (int i = 0; i < MAX_SENSOR_NODES; i++) {
    if (!weather_cache[i].valid) {
      continue;
    }

    uint8_t section_id =
      weather_cache[i].section_id;

    if (
      section_id >= MAX_SECTIONS ||
      !sections[section_id].active
    ) {
      continue;
    }

    control_payload_t control;

    if (manual_controls[section_id].active) {
      control = manual_controls[section_id].control;
      Serial.printf(
        "Using manual override for section %d\n",
        section_id
      );
    } else {
      control =
        run_ai_model(
          weather_cache[i].latest
        );
    }

    uint8_t section_mac[6];

    memcpy(
      section_mac,
      sections[section_id].mac,
      6
    );

    send_control_packet(
      section_mac,
      section_id,
      control
    );
  }
}

/* ============================
   SETUP
============================ */

void setup() {
  Serial.begin(115200);

  init_rainfall_model();

  mqtt.setServer(
    THINGSBOARD_SERVER,
    THINGSBOARD_PORT
  );
  mqtt.setCallback(mqtt_callback);
  mqtt.setBufferSize(MQTT_BUFFER_SIZE, MQTT_BUFFER_SIZE);

  WiFi.mode(WIFI_STA);
  WiFi.setChannel(WIFI_CHANNEL);

  init_espnow();
  init_gsm_modem();

  Serial.println("Master Online");
}

/* ============================
   LOOP
============================ */

void loop() {
  unsigned long now = millis();

  if (now - last_control_cycle > CONTROL_INTERVAL_MS) {
    process_control_cycle();
    last_control_cycle = now;
  }

  maintain_thingsboard();
  publish_queued_telemetry();
}
