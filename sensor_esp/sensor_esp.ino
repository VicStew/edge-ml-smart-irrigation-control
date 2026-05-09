#include <WiFi.h>
#include <esp_now.h>
#include <esp_mac.h>

/* ============================
   CONFIGURATION
============================ */

#define WIFI_CHANNEL 4

#define NODE_ID 1
#define SECTION_ID 1

#define SEND_INTERVAL_MS 5000
#define HEARTBEAT_INTERVAL_MS 15000

/* ============================
   SECTION NODE MAC
============================ */

uint8_t section_mac[6] = {
  0x24, 0x6F, 0x28, 0x11, 0x22, 0x33
};

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

  uint32_t sequence;
  msg_type_t type;

  union {
    sensor_payload_t sensor;
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

/* ============================
   SENSOR DATA GENERATOR
   Replace with real sensors later
============================ */

sensor_payload_t read_sensors() {
  sensor_payload_t s;

  s.moisture = 20 + random(60);
  s.temperature = 18 + random(15);
  s.humidity = 40 + random(50);

  s.pH = 5.5 + (random(30) / 10.0);

  s.nitrogen = random(100);
  s.phosphorus = random(100);
  s.potassium = random(100);

  s.light = random(1000);

  return s;
}

/* ============================
   SEND SENSOR PACKET
============================ */

void send_sensor_packet() {
  farm_packet_t pkt;

  WiFi.macAddress(pkt.source_mac);
  memcpy(pkt.destination_mac, section_mac, 6);

  pkt.section_id = SECTION_ID;
  pkt.node_id = NODE_ID;

  pkt.sequence = packet_counter++;
  pkt.type = MSG_SENSOR_DATA;

  pkt.sensor = read_sensors();

  esp_err_t result =
    esp_now_send(
      section_mac,
      (uint8_t*)&pkt,
      sizeof(pkt)
    );

  if (result == ESP_OK) {
    packets_sent++;

    Serial.println(
      "Sensor packet sent"
    );

    Serial.printf(
      "Moisture: %.2f\n",
      pkt.sensor.moisture
    );

    Serial.printf(
      "Temperature: %.2f\n",
      pkt.sensor.temperature
    );

    Serial.printf(
      "Humidity: %.2f\n",
      pkt.sensor.humidity
    );

    Serial.printf(
      "pH: %.2f\n",
      pkt.sensor.pH
    );
  } else {
    Serial.println(
      "Failed to send sensor packet"
    );
  }
}

/* ============================
   SEND HEARTBEAT
============================ */

void send_heartbeat() {
  farm_packet_t pkt;

  WiFi.macAddress(pkt.source_mac);
  memcpy(pkt.destination_mac, section_mac, 6);

  pkt.section_id = SECTION_ID;
  pkt.node_id = NODE_ID;

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

  Serial.println(
    "Heartbeat sent"
  );
}

/* ============================
   HANDLE CONTROL COMMANDS
============================ */

void handle_control_packet(
  farm_packet_t *pkt
) {
  Serial.println(
    "Control command received"
  );

  if (pkt->control.irrigate) {
    Serial.println(
      "Irrigation scheduled"
    );
  }

  if (
    pkt->control.spray_pesticide
  ) {
    Serial.println(
      "Pesticide scheduled"
    );
  }

  if (
    pkt->control.apply_fertilizer
  ) {
    Serial.println(
      "Fertilizer scheduled"
    );
  }
}

/* ============================
   HANDLE ACK
============================ */

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
      handle_control_packet(
        pkt
      );
      break;

    case MSG_ACK:
      handle_ack_packet(
        pkt
      );
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
   Arduino ESP32 v3+
============================ */

void on_data_sent(
  const wifi_tx_info_t *tx_info,
  esp_now_send_status_t status
) {
  Serial.print(
    "Send Status: "
  );

  if (
    status ==
    ESP_NOW_SEND_SUCCESS
  ) {
    Serial.println(
      "Success"
    );
  } else {
    Serial.println(
      "Failed"
    );
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
  farm_packet_t pkt;

  memcpy(
    &pkt,
    incoming_data,
    sizeof(pkt)
  );

  Serial.print(
    "Packet received from: "
  );

  print_mac(
    recv_info->src_addr
  );

  route_packet(
    &pkt
  );
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

  esp_now_register_send_cb(
    on_data_sent
  );

  esp_now_register_recv_cb(
    on_data_recv
  );

  esp_now_peer_info_t peerInfo = {};

  memcpy(
    peerInfo.peer_addr,
    section_mac,
    6
  );

  peerInfo.channel =
    WIFI_CHANNEL;

  peerInfo.encrypt =
    false;

  if (
    esp_now_add_peer(
      &peerInfo
    ) != ESP_OK
  ) {
    Serial.println(
      "Failed to add section peer"
    );
  }
}

/* ============================
   SETUP
============================ */

void setup() {
  Serial.begin(
    115200
  );

  WiFi.mode(
    WIFI_STA
  );

  WiFi.setChannel(
    WIFI_CHANNEL
  );

  init_espnow();

  Serial.println(
    "Sensor Node Online"
  );

  Serial.print(
    "Section MAC: "
  );

  print_mac(
    section_mac
  );
}

/* ============================
   LOOP
============================ */

void loop() {
  unsigned long now =
    millis();

  if (
    now - last_send >
    SEND_INTERVAL_MS
  ) {
    send_sensor_packet();

    last_send = now;
  }

  if (
    now - last_heartbeat >
    HEARTBEAT_INTERVAL_MS
  ) {
    send_heartbeat();

    last_heartbeat = now;
  }
}