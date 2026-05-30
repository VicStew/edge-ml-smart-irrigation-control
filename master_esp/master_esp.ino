#include <WiFi.h>
#include <esp_now.h>
#include <math.h>
#include <string.h>

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

/* ============================
   SECTION NODE TABLE
============================ */

typedef struct {
  uint8_t mac[6];
  bool active;
} section_node_t;

section_node_t sections[MAX_SECTIONS];

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
}

/* ============================
   RECEIVE CALLBACK
============================ */

void on_data_recv(
  const esp_now_recv_info_t *recv_info,
  const uint8_t *incoming_data,
  int len
) {
  if (len != sizeof(farm_packet_t)) {
    Serial.println("Invalid packet size");
    return;
  }

  farm_packet_t pkt;
  memcpy(&pkt, incoming_data, sizeof(pkt));

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

    control_payload_t control =
      run_ai_model(
        weather_cache[i].latest
      );

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

  WiFi.mode(WIFI_STA);
  WiFi.setChannel(WIFI_CHANNEL);

  init_espnow();

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
}
