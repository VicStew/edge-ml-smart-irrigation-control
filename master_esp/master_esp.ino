#include <WiFi.h>
#include <esp_now.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

/* ============================
   CONFIGURATION
============================ */

#define WIFI_CHANNEL 4
#define WEATHER_UPDATE_INTERVAL_MS 900000   // 15 minutes
#define CONTROL_INTERVAL_MS 30000           // 30 seconds

#define MAX_SECTIONS 10
#define MAX_SENSOR_NODES 32

const char* WIFI_SSID = "Batteries";
const char* WIFI_PASS = "12345678";

/* ============================
   MESSAGE TYPES
============================ */

typedef enum {
  MSG_SENSOR_DATA = 1,
  MSG_CONTROL_CMD = 2,
  MSG_STATUS = 3,
  MSG_ACK = 4
} msg_type_t;

/* ============================
   SENSOR PAYLOAD
============================ */

typedef struct {
  float moisture;
  float temperature;
  float humidity;
  float pH;
  float nitrogen;
  float phosphorus;
  float potassium;
  float light;
} sensor_payload_t;

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
    sensor_payload_t sensor;
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
   SENSOR CACHE
============================ */

typedef struct {
  bool valid;
  sensor_payload_t data;
} sensor_cache_t;

sensor_cache_t sensor_cache[MAX_SENSOR_NODES];

/* ============================
   WEATHER CACHE
============================ */

float weather_rain_probability = 0;
float weather_temperature = 0;

/* ============================
   TIMERS
============================ */

unsigned long last_weather_update = 0;
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

/* ============================
   WEATHER FETCH
============================ */

void fetch_weather() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected");
    return;
  }

  HTTPClient http;

  String url =
    "https://api.open-meteo.com/v1/forecast?"
    "latitude=-1.286389&longitude=36.817223"
    "&hourly=precipitation_probability,temperature_2m";

  http.begin(url);

  int httpCode = http.GET();

  if (httpCode == 200) {
    String payload = http.getString();

    DynamicJsonDocument doc(8192);
    deserializeJson(doc, payload);

    weather_rain_probability =
      doc["hourly"]["precipitation_probability"][0];

    weather_temperature =
      doc["hourly"]["temperature_2m"][0];

    Serial.printf(
      "Weather updated: Rain=%.2f Temp=%.2f\n",
      weather_rain_probability,
      weather_temperature
    );
  }

  http.end();
}

/* ============================
   AI MODEL (PLACEHOLDER)
============================ */

control_payload_t run_ai_model(
  sensor_payload_t sensor
) {
  control_payload_t control;

  control.irrigate = false;
  control.spray_pesticide = false;
  control.apply_fertilizer = false;
  control.irrigation_duration_sec = 0;

  // Irrigation logic
  if (
    sensor.moisture < 35 &&
    weather_rain_probability < 40
  ) {
    control.irrigate = true;
    control.irrigation_duration_sec = 120;
  }

  // Fertilizer logic
  if (
    sensor.nitrogen < 20 ||
    sensor.phosphorus < 20 ||
    sensor.potassium < 20
  ) {
    control.apply_fertilizer = true;
  }

  // Spray logic
  if (
    sensor.humidity > 80 &&
    sensor.temperature > 28
  ) {
    control.spray_pesticide = true;
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
   HANDLE SENSOR PACKET
============================ */

void handle_sensor_packet(
  farm_packet_t *pkt
) {
  sensor_cache[pkt->node_id].valid = true;
  sensor_cache[pkt->node_id].data = pkt->sensor;

  Serial.printf(
    "Sensor packet from Node %d\n",
    pkt->node_id
  );
}

/* ============================
   HANDLE STATUS PACKET
============================ */

void handle_status_packet(
  farm_packet_t *pkt
) {
  Serial.printf(
    "Section %d status:\n",
    pkt->section_id
  );

  Serial.printf(
    "Valve: %s\n",
    pkt->status.valve_state ?
    "ON" : "OFF"
  );

  Serial.printf(
    "Spray: %s\n",
    pkt->status.spray_state ?
    "ON" : "OFF"
  );

  Serial.printf(
    "Fertilizer: %s\n",
    pkt->status.fertilizer_state ?
    "ON" : "OFF"
  );
}

/* ============================
   RECEIVE CALLBACK
============================ */

void on_data_recv(
  const esp_now_recv_info_t *recv_info,
  const uint8_t *incoming_data,
  int len
) {
  farm_packet_t pkt;

  memcpy(
    &pkt,
    incoming_data,
    sizeof(pkt)
  );

  switch (pkt.type) {
    case MSG_SENSOR_DATA:
      handle_sensor_packet(&pkt);
      break;

    case MSG_STATUS:
      handle_status_packet(&pkt);
      break;

    default:
      Serial.println(
        "Unknown packet type"
      );
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
        Serial.printf(
            "Address received the packet: %d\n",
            tx_info->des_addr
        );
    }
}

/* ============================
   REGISTER SECTION NODE
============================ */

void add_section_peer(
  uint8_t *mac
) {
  esp_now_peer_info_t peerInfo = {};

  memcpy(
    peerInfo.peer_addr,
    mac,
    6
  );

  peerInfo.channel = WIFI_CHANNEL;
  peerInfo.encrypt = false;

  if (
    esp_now_add_peer(&peerInfo)
    == ESP_OK
  ) {
    Serial.println(
      "Section peer added"
    );
  }
}

/* ============================
   INIT ESPNOW
============================ */

void init_espnow() {
  if (
    esp_now_init()
    != ESP_OK
  ) {
    Serial.println(
      "ESP-NOW init failed"
    );
    ESP.restart();
  }

  esp_now_register_recv_cb(
    on_data_recv
  );

  esp_now_register_send_cb(
    on_data_sent
  );
}

/* ============================
   INIT WIFI
============================ */

void init_wifi() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(
    WIFI_SSID,
    WIFI_PASS
  );

  while (
    WiFi.status()
    != WL_CONNECTED
  ) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();
  Serial.println(
    "WiFi connected"
  );
}

/* ============================
   CONTROL LOOP
============================ */

void process_control_cycle() {
  for (
    int i = 0;
    i < MAX_SENSOR_NODES;
    i++
  ) {
    if (
      sensor_cache[i].valid
    ) {
      control_payload_t control =
        run_ai_model(
          sensor_cache[i].data
        );

      uint8_t section_mac[6];

      memcpy(
        section_mac,
        sections[i].mac,
        6
      );

      send_control_packet(
        section_mac,
        i,
        control
      );
    }
  }
}

/* ============================
   SETUP
============================ */

void setup() {
  Serial.begin(115200);

  init_wifi();

  WiFi.setChannel(
    WIFI_CHANNEL
  );

  init_espnow();

  Serial.println(
    "Orchestrator Online"
  );
}

/* ============================
   LOOP
============================ */

void loop() {
  unsigned long now =
    millis();

  if (
    now -
    last_weather_update >
    WEATHER_UPDATE_INTERVAL_MS
  ) {
    fetch_weather();

    last_weather_update =
      now;
  }

  if (
    now -
    last_control_cycle >
    CONTROL_INTERVAL_MS
  ) {
    process_control_cycle();

    last_control_cycle =
      now;
  }
}