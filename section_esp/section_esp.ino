#include <WiFi.h>
#include <esp_now.h>
#include <esp_mac.h>

/* ============================
   CONFIGURATION
============================ */

#define WIFI_CHANNEL 4
#define SECTION_ID 1

#define VALVE_PIN 4
#define SPRAY_PIN 5
#define FERTILIZER_PIN 6

#define STATUS_INTERVAL_MS 10000

/* ============================
   ORCHESTRATOR MAC
============================ */

uint8_t orchestrator_mac[6] = {
  0x24, 0x6F, 0x28, 0xAA, 0xBB, 0xCC
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
    sensor_payload_t sensor;
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
   SEND STATUS TO ORCHESTRATOR
============================ */

void send_status() {
  farm_packet_t pkt;

  WiFi.macAddress(pkt.source_mac);
  memcpy(pkt.destination_mac, orchestrator_mac, 6);

  pkt.section_id = SECTION_ID;
  pkt.node_id = 0;

  pkt.sequence = packet_counter++;
  pkt.type = MSG_STATUS;

  pkt.status.valve_state = valve_state;
  pkt.status.spray_state = spray_state;
  pkt.status.fertilizer_state = fertilizer_state;

  esp_now_send(
    orchestrator_mac,
    (uint8_t*)&pkt,
    sizeof(pkt)
  );

  Serial.println("Status sent to orchestrator");
}

/* ============================
   HANDLE SENSOR DATA
============================ */

void handle_sensor_packet(farm_packet_t *pkt) {
  Serial.println("Sensor packet received");

  Serial.printf("Node ID: %d\n", pkt->node_id);
  Serial.printf("Moisture: %.2f\n", pkt->sensor.moisture);
  Serial.printf("Temperature: %.2f\n", pkt->sensor.temperature);
  Serial.printf("Humidity: %.2f\n", pkt->sensor.humidity);
  Serial.printf("pH: %.2f\n", pkt->sensor.pH);

  /* Forward upstream */
  esp_now_send(
    orchestrator_mac,
    (uint8_t*)pkt,
    sizeof(*pkt)
  );

  Serial.println("Forwarded sensor data to orchestrator");
}

/* ============================
   HANDLE CONTROL COMMANDS
============================ */

void handle_control_packet(farm_packet_t *pkt) {
  Serial.println("Control packet received");

  valve_state = pkt->control.irrigate;
  spray_state = pkt->control.spray_pesticide;
  fertilizer_state = pkt->control.apply_fertilizer;

  digitalWrite(
    VALVE_PIN,
    valve_state ? HIGH : LOW
  );

  digitalWrite(
    SPRAY_PIN,
    spray_state ? HIGH : LOW
  );

  digitalWrite(
    FERTILIZER_PIN,
    fertilizer_state ? HIGH : LOW
  );

  Serial.printf(
    "Valve: %s\n",
    valve_state ? "ON" : "OFF"
  );

  Serial.printf(
    "Spray: %s\n",
    spray_state ? "ON" : "OFF"
  );

  Serial.printf(
    "Fertilizer: %s\n",
    fertilizer_state ? "ON" : "OFF"
  );

  if (valve_state) {
    delay(
      pkt->control.irrigation_duration_sec * 1000
    );

    valve_state = false;

    digitalWrite(
      VALVE_PIN,
      LOW
    );

    Serial.println("Irrigation cycle complete");
  }

  send_status();
}

/* ============================
   ROUTER
============================ */

void route_packet(farm_packet_t *pkt) {
  switch (pkt->type) {

    case MSG_SENSOR_DATA:
      handle_sensor_packet(pkt);
      break;

    case MSG_CONTROL_CMD:
      handle_control_packet(pkt);
      break;

    case MSG_STATUS:
      Serial.println("Status packet ignored");
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

  if (tx_info) {
        Serial.printf(
            "Address received the packet: %d\n",
            tx_info->des_addr
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

  Serial.print("Packet from: ");
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

  esp_now_register_send_cb(
    on_data_sent
  );

  esp_now_register_recv_cb(
    on_data_recv
  );

  esp_now_peer_info_t peerInfo = {};
  memcpy(
    peerInfo.peer_addr,
    orchestrator_mac,
    6
  );

  peerInfo.channel = WIFI_CHANNEL;
  peerInfo.encrypt = false;

  if (
    esp_now_add_peer(&peerInfo)
    != ESP_OK
  ) {
    Serial.println(
      "Failed to add orchestrator peer"
    );
  }
}

/* ============================
   GPIO INIT
============================ */

void init_gpio() {
  pinMode(
    VALVE_PIN,
    OUTPUT
  );

  pinMode(
    SPRAY_PIN,
    OUTPUT
  );

  pinMode(
    FERTILIZER_PIN,
    OUTPUT
  );

  digitalWrite(
    VALVE_PIN,
    LOW
  );

  digitalWrite(
    SPRAY_PIN,
    LOW
  );

  digitalWrite(
    FERTILIZER_PIN,
    LOW
  );
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

  Serial.println(
    "Section Node Online"
  );

  Serial.print(
    "Orchestrator MAC: "
  );

  print_mac(
    orchestrator_mac
  );
}

/* ============================
   LOOP
============================ */

void loop() {
  unsigned long now = millis();

  if (
    now - last_status_time >
    STATUS_INTERVAL_MS
  ) {
    send_status();
    last_status_time = now;
  }
}