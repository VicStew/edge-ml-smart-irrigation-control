#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define TINY_GSM_MODEM_SIM800
#define TINY_GSM_YIELD_MS 1
#define SerialAT Serial1

#include <ArduinoJson.h>
#include <TinyGsmClient.h>
#include <PubSubClient.h>

/* ============================
   CONFIGURATION
============================ */

#define WIFI_CHANNEL 4
#define CONTROL_INTERVAL_MS 30000
#define SOIL_MOISTURE_IRRIGATION_THRESHOLD_PERCENT 35.0f
#define MIN_IRRIGATION_DURATION_SEC 60
#define MAX_IRRIGATION_DURATION_SEC 300

#define MAX_SECTIONS 10
#define MAX_SENSOR_NODES 32

#define SIM800_RX_PIN 16
#define SIM800_TX_PIN 17
#define SIM800_BAUD 9600

#define THINGSBOARD_PORT 1883
#define MQTT_BUFFER_SIZE 768
#define TELEMETRY_QUEUE_LENGTH 8
#define TELEMETRY_PAYLOAD_SIZE 512
#define ESPNOW_QUEUE_LENGTH 16
#define DEVICE_CLIENT_ID_LENGTH 40
#define GATEWAY_DEVICE_NAME_LENGTH 48
#define MAX_CONNECTED_GATEWAY_DEVICES 64
#define GSM_RECONNECT_INTERVAL_MS 30000
#define MQTT_RECONNECT_INTERVAL_MS 10000
#define CLOUD_TASK_STACK_SIZE 8192
#define FARM_TASK_STACK_SIZE 6144
#define CLOUD_TASK_PRIORITY 1
#define FARM_TASK_PRIORITY 2
#define CLOUD_TASK_CORE 1
#define FARM_TASK_CORE 1

const char GSM_PIN[] = "";
const char GPRS_APN[] = "internet";
const char GPRS_USER[] = "";
const char GPRS_PASS[] = "";

const char THINGSBOARD_SERVER[] = "mqtt.eu.thingsboard.cloud";
const char MASTER_NODE_CLIENT_ID[] = "h0kqq9jqtrpufa6fxk9g";

const char TB_TELEMETRY_TOPIC[] = "v1/devices/me/telemetry";
const char TB_GATEWAY_CONNECT_TOPIC[] = "v1/gateway/connect";
const char TB_GATEWAY_TELEMETRY_TOPIC[] = "v1/gateway/telemetry";
const char TB_GATEWAY_ATTRIBUTES_TOPIC[] = "v1/gateway/attributes";
const char TB_GATEWAY_ATTRIBUTES_REQUEST_TOPIC[] =
  "v1/gateway/attributes/request";
const char TB_GATEWAY_ATTRIBUTES_RESPONSE_TOPIC[] =
  "v1/gateway/attributes/response";

/* ============================
   MESSAGE TYPES
============================ */

enum msg_type_t : uint8_t {
  MSG_SENSOR_DATA = 1,
  MSG_SECTION_DATA = 2,
  MSG_CONTROL_CMD = 3,
  MSG_STATUS = 4,
  MSG_ACK = 5
};

enum sensor_valid_flag_t : uint8_t {
  SENSOR_AMBIENT_VALID = 1 << 0,
  SENSOR_SOIL_MOISTURE_VALID = 1 << 1,
  SENSOR_SOIL_TEMPERATURE_VALID = 1 << 2
};

/* ============================
   SENSOR PAYLOADS
============================ */

typedef struct {
  uint32_t sample_time_ms;
  float ambient_temperature_c;
  float ambient_humidity_percent;
  uint16_t soil_moisture_adc;
  float soil_moisture_percent;
  float soil_temperature_c;
  uint8_t valid_fields;
} __attribute__((packed)) sensor_readings_t;

typedef struct {
  sensor_readings_t sensors;
  float water_flow_rate_l_min;
  float total_water_volume_l;
} __attribute__((packed)) section_readings_t;

/* ============================
   CONTROL PAYLOAD
============================ */

typedef struct {
  bool irrigate;
  uint16_t irrigation_duration_sec;
} __attribute__((packed)) control_payload_t;

/* ============================
   STATUS PAYLOAD
============================ */

enum valve_state_t : uint8_t {
  VALVE_UNKNOWN = 0,
  VALVE_CLOSED = 1,
  VALVE_OPENING = 2,
  VALVE_OPEN = 3,
  VALVE_CLOSING = 4
};

typedef struct {
  bool alive;
  valve_state_t valve_state;
  uint32_t uptime_ms;
  uint32_t packets_sent;
} __attribute__((packed)) status_payload_t;

/* ============================
   FARM PACKET
============================ */

typedef struct {
  uint8_t source_mac[6];
  uint8_t destination_mac[6];

  uint8_t section_id;
  uint8_t node_id;
  char device_client_id[DEVICE_CLIENT_ID_LENGTH];

  uint32_t sequence;
  msg_type_t type;

  union {
    sensor_readings_t sensor_data;
    section_readings_t section_data;
    control_payload_t control;
    status_payload_t status;
  };
} __attribute__((packed)) farm_packet_t;

static_assert(sizeof(sensor_readings_t) == 23, "Sensor payload layout changed");
static_assert(sizeof(section_readings_t) == 31, "Section payload layout changed");
static_assert(sizeof(control_payload_t) == 3, "Control payload layout changed");
static_assert(sizeof(status_payload_t) == 10, "Status payload layout changed");
static_assert(sizeof(farm_packet_t) == 90, "Farm packet layout changed");
static_assert(sizeof(farm_packet_t) <= ESP_NOW_MAX_DATA_LEN, "Farm packet is too large");

static const size_t FARM_PACKET_HEADER_SIZE =
  offsetof(farm_packet_t, sensor_data);

static const size_t FARM_PACKET_SENSOR_SIZE =
  FARM_PACKET_HEADER_SIZE +
  sizeof(sensor_readings_t);

static const size_t FARM_PACKET_SECTION_SIZE =
  FARM_PACKET_HEADER_SIZE +
  sizeof(section_readings_t);

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
   SENSOR READING CACHE
============================ */

typedef struct {
  bool valid;
  sensor_readings_t latest;
} sensor_cache_t;

sensor_cache_t sensor_cache[MAX_SECTIONS][MAX_SENSOR_NODES];
sensor_cache_t section_sensor_cache[MAX_SECTIONS];

/* ============================
   THINGSBOARD STATE
============================ */

TinyGsm modem(SerialAT);
TinyGsmClient gsm_client(modem);
PubSubClient mqtt(gsm_client);

typedef struct {
  bool gateway_device;
  bool accepts_valve_control;
  char device_name[GATEWAY_DEVICE_NAME_LENGTH];
  char payload[TELEMETRY_PAYLOAD_SIZE];
} telemetry_message_t;

typedef struct {
  bool active;
  char device_name[GATEWAY_DEVICE_NAME_LENGTH];
} connected_gateway_device_t;

typedef struct {
  farm_packet_t packet;
  uint8_t sender_mac[6];
} received_packet_t;

QueueHandle_t telemetry_queue = nullptr;
QueueHandle_t received_packet_queue = nullptr;
SemaphoreHandle_t espnow_send_mutex = nullptr;
portMUX_TYPE farm_state_mux = portMUX_INITIALIZER_UNLOCKED;

bool modem_ready = false;
uint32_t last_gsm_reconnect_attempt = 0;
uint32_t last_mqtt_reconnect_attempt = 0;
uint32_t shared_attribute_request_id = 1;
connected_gateway_device_t
  connected_gateway_devices[MAX_CONNECTED_GATEWAY_DEVICES];

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

void copy_text(
  char *destination,
  size_t destination_size,
  const char *source
) {
  if (destination_size == 0) {
    return;
  }

  if (!source) {
    source = "";
  }

  strncpy(
    destination,
    source,
    destination_size - 1
  );
  destination[destination_size - 1] = '\0';
}

bool text_is_empty(
  const char *text
) {
  return !text || text[0] == '\0';
}

void section_device_name(
  uint8_t section_id,
  char *destination,
  size_t destination_size
) {
  snprintf(
    destination,
    destination_size,
    "Section_%u",
    section_id
  );
}

void sensor_device_name(
  uint8_t section_id,
  uint8_t node_id,
  char *destination,
  size_t destination_size
) {
  snprintf(
    destination,
    destination_size,
    "Section_%u_Sensor_%u",
    section_id,
    node_id
  );
}

bool section_id_from_device_name(
  const char *device_name,
  uint8_t *section_id
) {
  const char prefix[] = "Section_";

  if (
    text_is_empty(device_name) ||
    strncmp(device_name, prefix, sizeof(prefix) - 1) != 0
  ) {
    return false;
  }

  char *end = nullptr;
  long parsed = strtol(
    device_name + sizeof(prefix) - 1,
    &end,
    10
  );

  if (
    end == nullptr ||
    *end != '\0' ||
    parsed < 0 ||
    parsed >= MAX_SECTIONS
  ) {
    return false;
  }

  *section_id = (uint8_t)parsed;
  return true;
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

const char *valve_state_name(valve_state_t state) {
  switch (state) {
    case VALVE_CLOSED:
      return "closed";
    case VALVE_OPENING:
      return "opening";
    case VALVE_OPEN:
      return "open";
    case VALVE_CLOSING:
      return "closing";
    default:
      return "unknown";
  }
}

bool add_section_peer(
  const uint8_t *mac
) {
  if (
    espnow_send_mutex == nullptr ||
    xSemaphoreTake(
      espnow_send_mutex,
      pdMS_TO_TICKS(100)
    ) != pdTRUE
  ) {
    Serial.println("ESP-NOW peer lock timeout");
    return false;
  }

  if (esp_now_is_peer_exist(mac)) {
    xSemaphoreGive(espnow_send_mutex);
    return true;
  }

  esp_now_peer_info_t peerInfo = {};

  memcpy(peerInfo.peer_addr, mac, 6);
  peerInfo.channel = WIFI_CHANNEL;
  peerInfo.encrypt = false;

  esp_err_t result = esp_now_add_peer(&peerInfo);
  xSemaphoreGive(espnow_send_mutex);

  if (result == ESP_OK) {
    Serial.println("Section peer added");
    return true;
  } else {
    Serial.println("Failed to add section peer");
    return false;
  }
}

bool enqueue_telemetry(
  const char *payload,
  const char *device_name,
  bool gateway_device,
  bool accepts_valve_control
) {
  if (text_is_empty(device_name)) {
    Serial.println("Telemetry device name missing");
    return false;
  }

  telemetry_message_t message = {};
  message.gateway_device = gateway_device;
  message.accepts_valve_control = accepts_valve_control;

  copy_text(
    message.device_name,
    sizeof(message.device_name),
    device_name
  );
  copy_text(
    message.payload,
    sizeof(message.payload),
    payload
  );

  if (
    telemetry_queue != nullptr &&
    xQueueSend(telemetry_queue, &message, 0) == pdTRUE
  ) {
    return true;
  }

  Serial.println("Telemetry queue full");
  return false;
}

void queue_sensor_telemetry(
  const farm_packet_t *pkt
) {
  StaticJsonDocument<TELEMETRY_PAYLOAD_SIZE> doc;

  doc["section_id"] = pkt->section_id;
  doc["node_id"] = pkt->node_id;
  doc["sample_time_ms"] = pkt->sensor_data.sample_time_ms;
  doc["ambient_temperature_c"] =
    pkt->sensor_data.ambient_temperature_c;
  doc["ambient_humidity_percent"] =
    pkt->sensor_data.ambient_humidity_percent;
  doc["soil_moisture_adc"] =
    pkt->sensor_data.soil_moisture_adc;
  doc["soil_moisture_percent"] =
    pkt->sensor_data.soil_moisture_percent;
  doc["soil_temperature_c"] =
    pkt->sensor_data.soil_temperature_c;
  doc["valid_fields"] = pkt->sensor_data.valid_fields;

  char payload[TELEMETRY_PAYLOAD_SIZE];
  size_t length =
    serializeJson(doc, payload, sizeof(payload));

  if (length == 0 || length >= sizeof(payload)) {
    Serial.println("Sensor telemetry payload too large");
    return;
  }

  char device_name[GATEWAY_DEVICE_NAME_LENGTH];
  sensor_device_name(
    pkt->section_id,
    pkt->node_id,
    device_name,
    sizeof(device_name)
  );

  if (enqueue_telemetry(payload, device_name, true, false)) {
    Serial.println("Sensor telemetry queued");
  }
}

void queue_section_sensor_telemetry(
  const farm_packet_t *pkt
) {
  StaticJsonDocument<TELEMETRY_PAYLOAD_SIZE> doc;

  doc["section_id"] = pkt->section_id;
  doc["node_id"] = pkt->node_id;
  doc["sample_time_ms"] =
    pkt->section_data.sensors.sample_time_ms;
  doc["ambient_temperature_c"] =
    pkt->section_data.sensors.ambient_temperature_c;
  doc["ambient_humidity_percent"] =
    pkt->section_data.sensors.ambient_humidity_percent;
  doc["soil_moisture_adc"] =
    pkt->section_data.sensors.soil_moisture_adc;
  doc["soil_moisture_percent"] =
    pkt->section_data.sensors.soil_moisture_percent;
  doc["soil_temperature_c"] =
    pkt->section_data.sensors.soil_temperature_c;
  doc["valid_fields"] =
    pkt->section_data.sensors.valid_fields;
  doc["water_flow_rate_l_min"] =
    pkt->section_data.water_flow_rate_l_min;
  doc["total_water_volume_l"] =
    pkt->section_data.total_water_volume_l;

  char payload[TELEMETRY_PAYLOAD_SIZE];
  size_t length =
    serializeJson(doc, payload, sizeof(payload));

  if (length == 0 || length >= sizeof(payload)) {
    Serial.println("Section sensor telemetry payload too large");
    return;
  }

  char device_name[GATEWAY_DEVICE_NAME_LENGTH];
  section_device_name(
    pkt->section_id,
    device_name,
    sizeof(device_name)
  );

  if (enqueue_telemetry(payload, device_name, true, true)) {
    Serial.println("Section sensor telemetry queued");
  }
}

void queue_section_status_telemetry(
  const farm_packet_t *pkt
) {
  StaticJsonDocument<384> doc;

  doc["section_id"] = pkt->section_id;
  doc["node_id"] = pkt->node_id;
  doc["alive"] = pkt->status.alive;
  doc["valve_state"] = valve_state_name(pkt->status.valve_state);
  doc["valve_open"] = pkt->status.valve_state == VALVE_OPEN;
  doc["uptime_ms"] = pkt->status.uptime_ms;
  doc["packets_sent"] = pkt->status.packets_sent;

  char payload[384];
  size_t length =
    serializeJson(doc, payload, sizeof(payload));

  if (length == 0 || length >= sizeof(payload)) {
    Serial.println("Status telemetry payload too large");
    return;
  }

  char device_name[GATEWAY_DEVICE_NAME_LENGTH];
  section_device_name(
    pkt->section_id,
    device_name,
    sizeof(device_name)
  );

  enqueue_telemetry(payload, device_name, true, true);
}

/* ============================
   IRRIGATION CONTROL ALGORITHM
============================ */

control_payload_t calculate_irrigation_control(
  const sensor_readings_t &sensors
) {
  control_payload_t control = {};

  if (
    (sensors.valid_fields & SENSOR_SOIL_MOISTURE_VALID) == 0
  ) {
    Serial.println("Skipping automatic irrigation: soil moisture invalid");
    return control;
  }

  float soil_moisture_percent =
    clamp_float(sensors.soil_moisture_percent, 0.0f, 100.0f);

  if (
    soil_moisture_percent <=
    SOIL_MOISTURE_IRRIGATION_THRESHOLD_PERCENT
  ) {
    float dryness =
      (
        SOIL_MOISTURE_IRRIGATION_THRESHOLD_PERCENT -
        soil_moisture_percent
      ) / SOIL_MOISTURE_IRRIGATION_THRESHOLD_PERCENT;

    control.irrigate = true;
    control.irrigation_duration_sec =
      MIN_IRRIGATION_DURATION_SEC +
      (uint16_t)(dryness * (
        MAX_IRRIGATION_DURATION_SEC -
        MIN_IRRIGATION_DURATION_SEC
      ));
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
  farm_packet_t pkt = {};

  WiFi.macAddress(pkt.source_mac);
  memcpy(pkt.destination_mac, section_mac, 6);

  pkt.section_id = section_id;
  pkt.node_id = 0;
  copy_text(
    pkt.device_client_id,
    sizeof(pkt.device_client_id),
    MASTER_NODE_CLIENT_ID
  );

  pkt.sequence = sequence_counter++;
  pkt.type = MSG_CONTROL_CMD;
  pkt.control = control;

  esp_err_t result = ESP_ERR_TIMEOUT;

  if (
    espnow_send_mutex != nullptr &&
    xSemaphoreTake(
      espnow_send_mutex,
      pdMS_TO_TICKS(100)
    ) == pdTRUE
  ) {
    result = esp_now_send(
      section_mac,
      (uint8_t*)&pkt,
      sizeof(pkt)
    );
    xSemaphoreGive(espnow_send_mutex);
  }

  Serial.printf(
    "Control %s to section %d\n",
    result == ESP_OK ? "sent" : "failed",
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
    enqueue_telemetry(
      payload,
      MASTER_NODE_CLIENT_ID,
      false,
      false
    );
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

  bool section_active;
  uint8_t section_mac[6] = {};

  portENTER_CRITICAL(&farm_state_mux);

  if (manual_enabled) {
    manual_controls[section_id].active = true;
    manual_controls[section_id].control = control;
  } else {
    manual_controls[section_id].active = false;

    control.irrigate = false;
    control.irrigation_duration_sec = 0;
  }

  section_active = sections[section_id].active;
  memcpy(section_mac, sections[section_id].mac, 6);

  portEXIT_CRITICAL(&farm_state_mux);

  if (!section_active) {
    Serial.printf(
      "Section %d is not discovered yet\n",
      section_id
    );
    return false;
  }

  send_control_packet(
    section_mac,
    section_id,
    control
  );

  return true;
}

void process_gateway_manual_control(
  uint8_t section_id,
  JsonObjectConst object
) {
  bool manual_enabled =
    json_bool_key(
      object,
      "enabled",
      json_bool_key(object, "manual", true)
    );

  control_payload_t control = {};
  control.irrigate =
    json_bool_key(
      object,
      "irrigate",
      json_bool_key(object, "valve", false)
    );

  int duration =
    json_int_key(
      object,
      "irrigation_duration_sec",
      json_int_key(object, "duration_sec", 0)
    );
  control.irrigation_duration_sec =
    duration > 0 ? (uint16_t)duration : 0;

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
}

void process_section_valve_state(
  uint8_t section_id,
  JsonVariantConst valve_state,
  const char *attribute_name
) {
  if (!valve_state.is<bool>()) {
    Serial.printf(
      "Ignoring %s for section %u: expected true or false\n",
      attribute_name,
      section_id
    );
    return;
  }

  control_payload_t control = {};
  control.irrigate = valve_state.as<bool>();

  bool forwarded =
    forward_manual_control(
      section_id,
      true,
      control
    );

  publish_manual_control_result(
    section_id,
    true,
    forwarded
  );

  Serial.printf(
    "%s=%s; section %u valve command %s\n",
    attribute_name,
    control.irrigate ? "true" : "false",
    section_id,
    forwarded ? "forwarded" : "queued until discovery"
  );
}

void process_gateway_attributes(
  JsonObjectConst root
) {
  const char *device_name = root["device"] | "";
  uint8_t section_id = 0;

  if (!section_id_from_device_name(device_name, &section_id)) {
    Serial.printf(
      "Ignoring gateway attributes for non-section device: %s\n",
      device_name
    );
    return;
  }

  JsonObjectConst attributes;

  if (root["data"].is<JsonObjectConst>()) {
    attributes = root["data"].as<JsonObjectConst>();
  } else if (root["value"].is<JsonObjectConst>()) {
    attributes = root["value"].as<JsonObjectConst>();
  } else if (root["values"].is<JsonObjectConst>()) {
    attributes = root["values"].as<JsonObjectConst>();
  } else if (root["shared"].is<JsonObjectConst>()) {
    attributes = root["shared"].as<JsonObjectConst>();
  } else {
    Serial.println("Gateway attribute message has no attribute object");
    return;
  }

  if (json_has_key(attributes, "valveState")) {
    process_section_valve_state(
      section_id,
      attributes["valveState"],
      "valveState"
    );
    return;
  }

  JsonVariantConst manual_control =
    attributes["manual_control"];

  if (!manual_control.is<JsonObjectConst>()) {
    manual_control = attributes["manualControl"];
  }

  if (manual_control.is<JsonObjectConst>()) {
    process_gateway_manual_control(
      section_id,
      manual_control.as<JsonObjectConst>()
    );
    return;
  }

  char legacy_key[32];
  snprintf(
    legacy_key,
    sizeof(legacy_key),
    "valveState_Section_%u",
    section_id
  );

  if (json_has_key(attributes, legacy_key)) {
    process_section_valve_state(
      section_id,
      attributes[legacy_key],
      legacy_key
    );
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

  if (
    strcmp(topic, TB_GATEWAY_ATTRIBUTES_TOPIC) != 0 &&
    strcmp(topic, TB_GATEWAY_ATTRIBUTES_RESPONSE_TOPIC) != 0
  ) {
    Serial.println("Ignoring unexpected ThingsBoard topic");
    return;
  }

  process_gateway_attributes(root);
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

void clear_connected_gateway_devices() {
  memset(
    connected_gateway_devices,
    0,
    sizeof(connected_gateway_devices)
  );
}

bool gateway_device_is_connected(
  const char *device_name
) {
  for (
    size_t i = 0;
    i < MAX_CONNECTED_GATEWAY_DEVICES;
    i++
  ) {
    if (
      connected_gateway_devices[i].active &&
      strcmp(
        connected_gateway_devices[i].device_name,
        device_name
      ) == 0
    ) {
      return true;
    }
  }

  return false;
}

void remember_connected_gateway_device(
  const char *device_name
) {
  for (
    size_t i = 0;
    i < MAX_CONNECTED_GATEWAY_DEVICES;
    i++
  ) {
    if (!connected_gateway_devices[i].active) {
      connected_gateway_devices[i].active = true;
      copy_text(
        connected_gateway_devices[i].device_name,
        sizeof(connected_gateway_devices[i].device_name),
        device_name
      );
      return;
    }
  }

  Serial.println("Connected gateway device table full");
}

bool request_gateway_shared_attributes(
  const char *device_name
) {
  char request[192];
  int length = snprintf(
    request,
    sizeof(request),
    "{\"id\":%lu,\"device\":\"%s\","
    "\"keys\":[\"valveState\",\"manual_control\","
    "\"manualControl\"],\"client\":false}",
    (unsigned long)shared_attribute_request_id++,
    device_name
  );

  return (
    length > 0 &&
    length < (int)sizeof(request) &&
    mqtt.publish(
      TB_GATEWAY_ATTRIBUTES_REQUEST_TOPIC,
      request
    )
  );
}

bool ensure_gateway_device_connected(
  const char *device_name,
  bool accepts_valve_control
) {
  if (gateway_device_is_connected(device_name)) {
    return true;
  }

  char request[96];
  int length = snprintf(
    request,
    sizeof(request),
    "{\"device\":\"%s\"}",
    device_name
  );

  if (
    length <= 0 ||
    length >= (int)sizeof(request) ||
    !mqtt.publish(TB_GATEWAY_CONNECT_TOPIC, request)
  ) {
    Serial.printf(
      "Failed to connect gateway device %s\n",
      device_name
    );
    return false;
  }

  if (
    accepts_valve_control &&
    !request_gateway_shared_attributes(device_name)
  ) {
    Serial.printf(
      "Failed to request shared attributes for %s\n",
      device_name
    );
    return false;
  }

  remember_connected_gateway_device(device_name);
  Serial.printf("Gateway device connected: %s\n", device_name);
  return true;
}

bool connect_thingsboard_gateway() {
  if (mqtt.connected()) {
    return true;
  }

  if (!connect_gprs()) {
    return false;
  }

  Serial.print("Connecting ThingsBoard MQTT: ");
  Serial.println(THINGSBOARD_SERVER);
  Serial.print("Gateway client_id: ");
  Serial.println(MASTER_NODE_CLIENT_ID);

  bool connected =
    mqtt.connect(
      MASTER_NODE_CLIENT_ID
    );

  if (!connected) {
    Serial.printf(
      "ThingsBoard MQTT failed, state=%d\n",
      mqtt.state()
    );
    return false;
  }

  clear_connected_gateway_devices();

  bool subscribed =
    mqtt.subscribe(TB_GATEWAY_ATTRIBUTES_TOPIC) &&
    mqtt.subscribe(TB_GATEWAY_ATTRIBUTES_RESPONSE_TOPIC);

  if (!subscribed) {
    Serial.println("ThingsBoard gateway subscription failed");
    mqtt.disconnect();
    return false;
  }

  Serial.println("ThingsBoard gateway MQTT connected");
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
  connect_thingsboard_gateway();
}

void publish_queued_telemetry() {
  if (
    !modem_ready ||
    !mqtt.connected() ||
    telemetry_queue == nullptr ||
    uxQueueMessagesWaiting(telemetry_queue) == 0
  ) {
    return;
  }

  telemetry_message_t message = {};

  if (xQueuePeek(telemetry_queue, &message, 0) != pdTRUE) {
    return;
  }

  const char *topic = TB_TELEMETRY_TOPIC;
  const char *payload = message.payload;
  char gateway_payload[MQTT_BUFFER_SIZE];

  if (message.gateway_device) {
    if (
      !ensure_gateway_device_connected(
        message.device_name,
        message.accepts_valve_control
      )
    ) {
      return;
    }

    int length = snprintf(
      gateway_payload,
      sizeof(gateway_payload),
      "{\"%s\":[%s]}",
      message.device_name,
      message.payload
    );

    if (
      length <= 0 ||
      length >= (int)sizeof(gateway_payload)
    ) {
      Serial.println("Gateway telemetry payload too large");
      xQueueReceive(telemetry_queue, &message, 0);
      return;
    }

    topic = TB_GATEWAY_TELEMETRY_TOPIC;
    payload = gateway_payload;
  }

  if (!mqtt.publish(topic, payload)) {
    Serial.println("Telemetry publish failed");
    return;
  }

  xQueueReceive(telemetry_queue, &message, 0);
  Serial.println("Telemetry published to ThingsBoard");
}

void register_section(
  uint8_t section_id,
  const uint8_t *section_mac
) {
  if (!add_section_peer(section_mac)) {
    return;
  }

  portENTER_CRITICAL(&farm_state_mux);
  memcpy(sections[section_id].mac, section_mac, 6);
  sections[section_id].active = true;
  portEXIT_CRITICAL(&farm_state_mux);
}

void print_sensor_readings(
  const sensor_readings_t &readings
) {
  Serial.printf(
    "ambient_temperature_c: %.2f\n",
    readings.ambient_temperature_c
  );
  Serial.printf(
    "ambient_humidity_percent: %.2f\n",
    readings.ambient_humidity_percent
  );
  Serial.printf(
    "soil_moisture_adc: %u\n",
    readings.soil_moisture_adc
  );
  Serial.printf(
    "soil_moisture_percent: %.2f\n",
    readings.soil_moisture_percent
  );
  Serial.printf(
    "soil_temperature_c: %.2f\n",
    readings.soil_temperature_c
  );
}

/* ============================
   HANDLE SENSOR PACKETS
============================ */

void handle_sensor_packet(
  farm_packet_t *pkt,
  const uint8_t *section_mac
) {
  pkt->device_client_id[DEVICE_CLIENT_ID_LENGTH - 1] = '\0';

  if (
    pkt->node_id >= MAX_SENSOR_NODES ||
    pkt->section_id >= MAX_SECTIONS
  ) {
    Serial.println("Sensor packet index out of range");
    return;
  }

  register_section(pkt->section_id, section_mac);

  sensor_cache_t *cache =
    &sensor_cache[pkt->section_id][pkt->node_id];
  cache->valid = true;
  cache->latest = pkt->sensor_data;

  Serial.printf(
    "Sensor packet from Section %d Node %d stored\n",
    pkt->section_id,
    pkt->node_id
  );
  Serial.print("Sensor client_id: ");
  Serial.println(pkt->device_client_id);
  // print_sensor_readings(pkt->sensor_data);

  queue_sensor_telemetry(pkt);
}

void handle_section_sensor_packet(
  farm_packet_t *pkt,
  const uint8_t *section_mac
) {
  pkt->device_client_id[DEVICE_CLIENT_ID_LENGTH - 1] = '\0';

  if (pkt->section_id >= MAX_SECTIONS) {
    Serial.println("Section sensor packet index out of range");
    return;
  }

  register_section(pkt->section_id, section_mac);

  sensor_cache_t *cache =
    &section_sensor_cache[pkt->section_id];
  cache->valid = true;
  cache->latest = pkt->section_data.sensors;

  Serial.printf(
    "Section %d sensor packet stored\n",
    pkt->section_id
  );
  Serial.print("Section client_id: ");
  Serial.println(pkt->device_client_id);
  // print_sensor_readings(pkt->section_data.sensors);
  // Serial.printf(
  //   "water_flow_rate_l_min: %.3f\n",
  //   pkt->section_data.water_flow_rate_l_min
  // );
  // Serial.printf(
  //   "total_water_volume_l: %.3f\n",
  //   pkt->section_data.total_water_volume_l
  // );

  queue_section_sensor_telemetry(pkt);
}

/* ============================
   HANDLE STATUS PACKET
============================ */

void handle_status_packet(
  farm_packet_t *pkt
) {
  pkt->device_client_id[DEVICE_CLIENT_ID_LENGTH - 1] = '\0';

  // Serial.printf("Section %d status:\n", pkt->section_id);
  // Serial.print("Section client_id: ");
  // Serial.println(pkt->device_client_id);
  // Serial.printf("Alive: %s\n", pkt->status.alive ? "YES" : "NO");
  // Serial.printf(
  //   "Valve: %s\n",
  //   valve_state_name(pkt->status.valve_state)
  // );
  // Serial.printf("Uptime: %lu ms\n", pkt->status.uptime_ms);
  // Serial.printf("Packets sent: %lu\n", pkt->status.packets_sent);

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
    case MSG_SENSOR_DATA:
      return len == FARM_PACKET_SENSOR_SIZE;

    case MSG_SECTION_DATA:
      return len == FARM_PACKET_SECTION_SIZE;

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

  received_packet_t received = {};
  size_t copy_len =
    len < (int)sizeof(received.packet) ?
    (size_t)len :
    sizeof(received.packet);

  memcpy(&received.packet, incoming_data, copy_len);
  memcpy(received.sender_mac, recv_info->src_addr, 6);

  if (!packet_size_is_valid(received.packet.type, (size_t)len)) {
    Serial.printf(
      "Invalid packet size: %d bytes for type %d\n",
      len,
      (int)received.packet.type
    );
    return;
  }

  if (
    received_packet_queue == nullptr ||
    xQueueSend(received_packet_queue, &received, 0) != pdTRUE
  ) {
    Serial.println("ESP-NOW receive queue full");
  }
}

void process_received_packet(
  received_packet_t *received
) {
  farm_packet_t *pkt = &received->packet;

  switch (pkt->type) {
    case MSG_SENSOR_DATA:
      handle_sensor_packet(
        pkt,
        received->sender_mac
      );
      break;

    case MSG_SECTION_DATA:
      handle_section_sensor_packet(
        pkt,
        received->sender_mac
      );
      break;

    case MSG_STATUS:
      handle_status_packet(pkt);
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
    while (true) {
      delay(1000);
    }
  }

  esp_now_register_recv_cb(on_data_recv);
  esp_now_register_send_cb(on_data_sent);
}

/* ============================
   CONTROL LOOP
============================ */

bool select_driest_section_reading(
  uint8_t section_id,
  sensor_readings_t *selected
) {
  bool found = false;

  if (
    section_sensor_cache[section_id].valid &&
    (
      section_sensor_cache[section_id].latest.valid_fields &
      SENSOR_SOIL_MOISTURE_VALID
    ) != 0
  ) {
    *selected = section_sensor_cache[section_id].latest;
    found = true;
  }

  for (int node_id = 0; node_id < MAX_SENSOR_NODES; node_id++) {
    sensor_cache_t *cache =
      &sensor_cache[section_id][node_id];

    if (
      !cache->valid ||
      (
        cache->latest.valid_fields &
        SENSOR_SOIL_MOISTURE_VALID
      ) == 0
    ) {
      continue;
    }

    if (
      !found ||
      cache->latest.soil_moisture_percent <
        selected->soil_moisture_percent
    ) {
      *selected = cache->latest;
      found = true;
    }
  }

  return found;
}

void process_control_cycle() {
  for (uint8_t section_id = 0; section_id < MAX_SECTIONS; section_id++) {
    bool section_active;
    manual_control_t manual_control;
    uint8_t section_mac[6];

    portENTER_CRITICAL(&farm_state_mux);
    section_active = sections[section_id].active;
    manual_control = manual_controls[section_id];
    memcpy(section_mac, sections[section_id].mac, 6);
    portEXIT_CRITICAL(&farm_state_mux);

    if (!section_active) {
      continue;
    }

    control_payload_t control = {};

    if (manual_control.active) {
      control = manual_control.control;
      Serial.printf(
        "Using manual override for section %d\n",
        section_id
      );
    } else {
      sensor_readings_t driest_reading = {};

      if (
        !select_driest_section_reading(
          section_id,
          &driest_reading
        )
      ) {
        Serial.printf(
          "No valid soil moisture for section %d\n",
          section_id
        );
        continue;
      }

      control = calculate_irrigation_control(driest_reading);

      Serial.printf(
        "Section %d driest soil moisture: %.2f%%\n",
        section_id,
        driest_reading.soil_moisture_percent
      );
    }

    send_control_packet(
      section_mac,
      section_id,
      control
    );
  }
}

/* ============================
   FREERTOS TASKS
============================ */

void farm_task(void *parameter) {
  received_packet_t received = {};

  for (;;) {
    if (
      xQueueReceive(
        received_packet_queue,
        &received,
        pdMS_TO_TICKS(10)
      ) == pdTRUE
    ) {
      process_received_packet(&received);
    }

    unsigned long now = millis();

    if (now - last_control_cycle >= CONTROL_INTERVAL_MS) {
      process_control_cycle();
      last_control_cycle = now;
    }
  }
}

void cloud_task(void *parameter) {
  init_gsm_modem();

  for (;;) {
    maintain_thingsboard();
    publish_queued_telemetry();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

/* ============================
   SETUP
============================ */

void setup() {
  Serial.begin(115200);

  telemetry_queue = xQueueCreate(
    TELEMETRY_QUEUE_LENGTH,
    sizeof(telemetry_message_t)
  );
  received_packet_queue = xQueueCreate(
    ESPNOW_QUEUE_LENGTH,
    sizeof(received_packet_t)
  );
  espnow_send_mutex = xSemaphoreCreateMutex();

  if (
    telemetry_queue == nullptr ||
    received_packet_queue == nullptr ||
    espnow_send_mutex == nullptr
  ) {
    Serial.println("Failed to create master FreeRTOS queues");
    while (true) {
      delay(1000);
    }
  }

  mqtt.setServer(
    THINGSBOARD_SERVER,
    THINGSBOARD_PORT
  );
  mqtt.setCallback(mqtt_callback);
  mqtt.setBufferSize(MQTT_BUFFER_SIZE, MQTT_BUFFER_SIZE);

  WiFi.mode(WIFI_STA);
  WiFi.setChannel(WIFI_CHANNEL);

  init_espnow();

  BaseType_t farm_task_created =
    xTaskCreatePinnedToCore(
      farm_task,
      "farm_task",
      FARM_TASK_STACK_SIZE,
      nullptr,
      FARM_TASK_PRIORITY,
      nullptr,
      FARM_TASK_CORE
    );

  BaseType_t cloud_task_created =
    xTaskCreatePinnedToCore(
      cloud_task,
      "cloud_task",
      CLOUD_TASK_STACK_SIZE,
      nullptr,
      CLOUD_TASK_PRIORITY,
      nullptr,
      CLOUD_TASK_CORE
    );

  if (
    farm_task_created != pdPASS ||
    cloud_task_created != pdPASS
  ) {
    Serial.println("Failed to create master FreeRTOS tasks");
    while (true) {
      delay(1000);
    }
  }

  Serial.println("Master Online");
}

/* ============================
   LOOP
============================ */

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
