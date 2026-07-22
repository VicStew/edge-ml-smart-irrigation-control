#include <WiFi.h>
#include <esp_now.h>
#include <esp_mac.h>
#include <math.h>
#include <string.h>

#include "recent_weather_sample.h"

/* ============================
   CONFIGURATION
============================ */

#define WIFI_CHANNEL 4

#define NODE_ID 1
#define SECTION_ID 1

#define SEND_INTERVAL_MS 5000
#define HEARTBEAT_INTERVAL_MS 15000
#define DEVICE_CLIENT_ID_LENGTH 40

const char SENSOR_NODE_CLIENT_ID[] = "";

/* ============================
   SECTION NODE MAC
============================ */

uint8_t section_mac[6] = {
  0x3C, 0xE9, 0x0E, 0x94, 0xA6, 0x88
};

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
  bool alive;
  uint32_t uptime;
  uint32_t packets_sent;
} status_payload_t;

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
    weather_payload_t weather;
    control_payload_t control;
    status_payload_t status;
  };

} __attribute__((packed)) farm_packet_t;

/* ============================
   GLOBAL STATE
============================ */

unsigned long last_send = 0;
unsigned long last_heartbeat = 0;

uint32_t packet_counter = 0;
uint32_t packets_sent = 0;
uint8_t weather_sample_index = 0;

/* ============================
   HELPERS
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

float calculate_vapour_pressure_deficit_kpa(
  float temperature_c,
  float relative_humidity_percent
) {
  float relative_humidity =
    clamp_float(
      relative_humidity_percent,
      0.0f,
      100.0f
    );

  float saturation_vapour_pressure_kpa =
    0.6108f *
    expf(
      (17.27f * temperature_c) /
      (temperature_c + 237.3f)
    );

  float vapour_pressure_deficit_kpa =
    saturation_vapour_pressure_kpa *
    (1.0f - (relative_humidity / 100.0f));

  return vapour_pressure_deficit_kpa > 0.0f ?
    vapour_pressure_deficit_kpa :
    0.0f;
}

weather_payload_t read_weather_sample() {
  recent_weather_sample_t sample =
    RECENT_WEATHER_SAMPLES[
      weather_sample_index
    ];

  weather_payload_t reading;
  reading.sample_time = sample.sample_time;
  reading.temperature_2m = sample.temperature_2m;
  reading.relative_humidity_2m = sample.relative_humidity_2m;
  reading.vapour_pressure_deficit_kpa =
    calculate_vapour_pressure_deficit_kpa(
      reading.temperature_2m,
      reading.relative_humidity_2m
    );
  reading.soil_temperature_0_to_7cm = sample.soil_temperature_0_to_7cm;
  reading.soil_moisture_0_to_7cm = sample.soil_moisture_0_to_7cm;
  reading.et0_fao_evapotranspiration = sample.et0_fao_evapotranspiration;
  reading.shortwave_radiation = sample.shortwave_radiation;

  weather_sample_index =
    (weather_sample_index + 1) %
    RECENT_WEATHER_SAMPLE_COUNT;

  return reading;
}

/* ============================
   SEND WEATHER PACKET
============================ */

void send_weather_packet() {
  farm_packet_t pkt = {};

  WiFi.macAddress(pkt.source_mac);
  memcpy(pkt.destination_mac, section_mac, 6);

  pkt.section_id = SECTION_ID;
  pkt.node_id = NODE_ID;
  copy_text(
    pkt.device_client_id,
    sizeof(pkt.device_client_id),
    SENSOR_NODE_CLIENT_ID
  );

  pkt.sequence = packet_counter++;
  pkt.type = MSG_WEATHER_DATA;
  pkt.weather = read_weather_sample();

  esp_err_t result =
    esp_now_send(
      section_mac,
      (uint8_t*)&pkt,
      sizeof(pkt)
    );

  if (result == ESP_OK) {
    packets_sent++;

    Serial.println("Weather packet sent");
    Serial.printf("temperature_2m: %.2f\n", pkt.weather.temperature_2m);
    Serial.printf("relative_humidity_2m: %.2f\n", pkt.weather.relative_humidity_2m);
    Serial.printf("vapour_pressure_deficit_kpa: %.3f\n", pkt.weather.vapour_pressure_deficit_kpa);
    Serial.printf("soil_temperature_0_to_7cm: %.2f\n", pkt.weather.soil_temperature_0_to_7cm);
    Serial.printf("soil_moisture_0_to_7cm: %.3f\n", pkt.weather.soil_moisture_0_to_7cm);
    Serial.printf("et0_fao_evapotranspiration: %.3f\n", pkt.weather.et0_fao_evapotranspiration);
    Serial.printf("shortwave_radiation: %.2f\n", pkt.weather.shortwave_radiation);
  } else {
    Serial.println("Failed to send weather packet");
  }
}

/* ============================
   SEND HEARTBEAT
============================ */

void send_heartbeat() {
  farm_packet_t pkt = {};

  WiFi.macAddress(pkt.source_mac);
  memcpy(pkt.destination_mac, section_mac, 6);

  pkt.section_id = SECTION_ID;
  pkt.node_id = NODE_ID;
  copy_text(
    pkt.device_client_id,
    sizeof(pkt.device_client_id),
    SENSOR_NODE_CLIENT_ID
  );

  pkt.sequence = packet_counter++;
  pkt.type = MSG_STATUS;

  pkt.status.alive = true;
  pkt.status.uptime = millis();
  pkt.status.packets_sent = packets_sent;

  esp_now_send(
    section_mac,
    (uint8_t*)&pkt,
    sizeof(pkt)
  );

  Serial.println("Heartbeat sent");
}

/* ============================
   HANDLE CONTROL COMMANDS
============================ */

void handle_control_packet(
  farm_packet_t *pkt
) {
  Serial.println("Control command received");

  if (pkt->control.irrigate) {
    Serial.printf(
      "Irrigation scheduled for %u sec\n",
      pkt->control.irrigation_duration_sec
    );
  }

  if (pkt->control.spray_pesticide) {
    Serial.println("Pesticide scheduled");
  }

  if (pkt->control.apply_fertilizer) {
    Serial.println("Fertilizer scheduled");
  }
}

void handle_ack_packet(
  farm_packet_t *pkt
) {
  Serial.printf(
    "ACK received seq=%lu\n",
    pkt->sequence
  );
}

/* ============================
   ROUTER
============================ */

void route_packet(
  farm_packet_t *pkt
) {
  switch (pkt->type) {
    case MSG_CONTROL_CMD:
      handle_control_packet(pkt);
      break;

    case MSG_ACK:
      handle_ack_packet(pkt);
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

  farm_packet_t pkt = {};
  memcpy(&pkt, incoming_data, sizeof(pkt));

  Serial.print("Packet received from: ");
  print_mac(recv_info->src_addr);

  route_packet(&pkt);
}

/* ============================
   INIT ESPNOW
============================ */

void init_espnow() {
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    ESP.restart();
  }

  esp_now_register_send_cb(on_data_sent);
  esp_now_register_recv_cb(on_data_recv);

  esp_now_peer_info_t peerInfo = {};

  memcpy(peerInfo.peer_addr, section_mac, 6);
  peerInfo.channel = WIFI_CHANNEL;
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Failed to add section peer");
  }
}

/* ============================
   SETUP
============================ */

void setup() {
  Serial.begin(115200);

  WiFi.mode(WIFI_STA);
  WiFi.setChannel(WIFI_CHANNEL);

  init_espnow();

  Serial.println("Sensor Node Online");
  Serial.print("Section MAC: ");
  print_mac(section_mac);
}

/* ============================
   LOOP
============================ */

void loop() {
  unsigned long now = millis();

  if (now - last_send > SEND_INTERVAL_MS) {
    send_weather_packet();
    last_send = now;
  }

  if (now - last_heartbeat > HEARTBEAT_INTERVAL_MS) {
    send_heartbeat();
    last_heartbeat = now;
  }
}
