#include <WiFi.h>
#include <esp_now.h>
#include <esp_mac.h>

/* ============================
   CONFIGURATION
============================ */

#define WIFI_CHANNEL 4
#define SECTION_ID 1

#define VALVE_PIN 25
#define SPRAY_PIN 26
#define FERTILIZER_PIN 27

#define STATUS_INTERVAL_MS 10000
#define MAX_SENSOR_PEERS 16

/* ============================
   MASTER MAC
============================ */

uint8_t master_mac[6] = {
  0x98, 0xA3, 0x16, 0xC9, 0x3F, 0x2C
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
  float soil_temperature_0_to_7cm;
  float et0_fao_evapotranspiration;
  float shortwave_radiation;
} weather_payload_t;

/* =========================
   CONTROL PAYLOAD
========================= */

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
   GLOBAL STATE
============================ */

bool valve_state = false;
bool spray_state = false;
bool fertilizer_state = false;

unsigned long last_status_time = 0;
uint32_t packet_counter = 0;

uint8_t sensor_peers[MAX_SENSOR_PEERS][6];
uint8_t sensor_peer_count = 0;

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

bool mac_equal(
  const uint8_t *left,
  const uint8_t *right
) {
  return memcmp(left, right, 6) == 0;
}

void add_peer_if_needed(
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
    Serial.print("Peer added: ");
    print_mac(mac);
  } else {
    Serial.println("Failed to add peer");
  }
}

void remember_sensor_peer(
  const uint8_t *mac
) {
  for (uint8_t i = 0; i < sensor_peer_count; i++) {
    if (mac_equal(sensor_peers[i], mac)) {
      return;
    }
  }

  if (sensor_peer_count >= MAX_SENSOR_PEERS) {
    Serial.println("Sensor peer table full");
    return;
  }

  memcpy(sensor_peers[sensor_peer_count], mac, 6);
  sensor_peer_count++;
  add_peer_if_needed(mac);
}

/* ============================
   SEND STATUS TO MASTER
============================ */

void send_status() {
  farm_packet_t pkt;

  WiFi.macAddress(pkt.source_mac);
  memcpy(pkt.destination_mac, master_mac, 6);

  pkt.section_id = SECTION_ID;
  pkt.node_id = 0;

  pkt.sequence = packet_counter++;
  pkt.type = MSG_STATUS;

  pkt.status.valve_state = valve_state;
  pkt.status.spray_state = spray_state;
  pkt.status.fertilizer_state = fertilizer_state;

  esp_now_send(
    master_mac,
    (uint8_t*)&pkt,
    sizeof(pkt)
  );

  Serial.println("Status sent to master");
}

/* ============================
   FORWARD WEATHER DATA
============================ */

void handle_weather_packet(
  farm_packet_t *pkt,
  const uint8_t *sensor_mac
) {
  remember_sensor_peer(sensor_mac);

  Serial.printf(
    "Weather packet from node %d\n",
    pkt->node_id
  );
  Serial.printf("temperature_2m: %.2f\n", pkt->weather.temperature_2m);
  Serial.printf("relative_humidity_2m: %.2f\n", pkt->weather.relative_humidity_2m);
  Serial.printf("soil_temperature_0_to_7cm: %.2f\n", pkt->weather.soil_temperature_0_to_7cm);
  Serial.printf("et0_fao_evapotranspiration: %.3f\n", pkt->weather.et0_fao_evapotranspiration);
  Serial.printf("shortwave_radiation: %.2f\n", pkt->weather.shortwave_radiation);

  WiFi.macAddress(pkt->source_mac);
  memcpy(pkt->destination_mac, master_mac, 6);
  pkt->section_id = SECTION_ID;

  esp_now_send(
    master_mac,
    (uint8_t*)pkt,
    sizeof(*pkt)
  );

  Serial.println("Forwarded weather data to master");
}

/* ============================
   HANDLE CONTROL COMMANDS
============================ */

void handle_control_packet(farm_packet_t *pkt) {
  Serial.println("Control packet received");

  valve_state = pkt->control.irrigate;
  spray_state = pkt->control.spray_pesticide;
  fertilizer_state = pkt->control.apply_fertilizer;

  digitalWrite(VALVE_PIN, valve_state ? HIGH : LOW);
  digitalWrite(SPRAY_PIN, spray_state ? HIGH : LOW);
  digitalWrite(FERTILIZER_PIN, fertilizer_state ? HIGH : LOW);

  Serial.printf("Valve: %s\n", valve_state ? "ON" : "OFF");
  Serial.printf("Spray: %s\n", spray_state ? "ON" : "OFF");
  Serial.printf("Fertilizer: %s\n", fertilizer_state ? "ON" : "OFF");

  if (valve_state) {
    delay(pkt->control.irrigation_duration_sec * 1000);

    valve_state = false;
    digitalWrite(VALVE_PIN, LOW);

    Serial.println("Irrigation cycle complete");
  }

  send_status();
}

/* ============================
   ROUTER
============================ */

void route_packet(
  farm_packet_t *pkt,
  const uint8_t *sender_mac
) {
  switch (pkt->type) {
    case MSG_WEATHER_DATA:
      handle_weather_packet(pkt, sender_mac);
      break;

    case MSG_CONTROL_CMD:
      handle_control_packet(pkt);
      break;

    case MSG_STATUS:
      Serial.println("Sensor heartbeat received");
      remember_sensor_peer(sender_mac);
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
    Serial.println("Fail");
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

  farm_packet_t pkt;
  memcpy(&pkt, incoming_data, sizeof(pkt));

  Serial.print("Packet from: ");
  print_mac(recv_info->src_addr);

  route_packet(&pkt, recv_info->src_addr);
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

  add_peer_if_needed(master_mac);
}

/* ============================
   GPIO INIT
============================ */

void init_gpio() {
  pinMode(VALVE_PIN, OUTPUT);
  pinMode(SPRAY_PIN, OUTPUT);
  pinMode(FERTILIZER_PIN, OUTPUT);

  digitalWrite(VALVE_PIN, LOW);
  digitalWrite(SPRAY_PIN, LOW);
  digitalWrite(FERTILIZER_PIN, LOW);
}

/* ============================
   SETUP
============================ */

void setup() {
  Serial.begin(115200);

  WiFi.mode(WIFI_STA);
  WiFi.setChannel(WIFI_CHANNEL);

  init_gpio();
  init_espnow();

  Serial.println("Section Node Online");
  Serial.print("Master MAC: ");
  print_mac(master_mac);
}

/* ============================
   LOOP
============================ */

void loop() {
  unsigned long now = millis();

  if (now - last_status_time > STATUS_INTERVAL_MS) {
    send_status();
    last_status_time = now;
  }
}
