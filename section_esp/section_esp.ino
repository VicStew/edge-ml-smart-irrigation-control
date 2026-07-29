#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_mac.h>
#include <math.h>
#include <string.h>

#include <dhtnew.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <FlowSensor.h>

/* ============================
   CONFIGURATION
============================ */

#define WIFI_CHANNEL 4
#define SECTION_ID 1

#define AM2301A_PIN 26
#define SOIL_MOISTURE_ADC_PIN 34
#define DS18B20_PIN 25
#define WATER_FLOW_PIN 32

#define VALVE_PIN 5

#define SOIL_MOISTURE_DRY_ADC 4095
#define SOIL_MOISTURE_WET_ADC 0
#define SOIL_MOISTURE_SAMPLE_COUNT 8

// The ZJ-G1 datasheet calibration is F(Hz) = 1 * Q(L/min), or 60 pulses/L.
// Replace this value with the result of a measured-volume calibration.
#define FLOW_SENSOR_PULSES_PER_LITER 60

#define SENSOR_INTERVAL_MS 5000
#define FLOW_READ_INTERVAL_MS 1000
#define STATUS_INTERVAL_MS 10000
#define MAX_SENSOR_PEERS 16
#define DEVICE_CLIENT_ID_LENGTH 40

const char SECTION_NODE_CLIENT_ID[] = "0trhe202q8u4wxegyzkf";

/* ============================
   MASTER MAC
============================ */

uint8_t master_mac[6] = {
  0xD4, 0xE9, 0xF4, 0x71, 0x97, 0x44
};

/* ============================
   SENSORS
============================ */

DHTNEW ambient_sensor(AM2301A_PIN);
OneWire soil_temperature_bus(DS18B20_PIN);
DallasTemperature soil_temperature_sensor(&soil_temperature_bus);
FlowSensor water_flow_sensor(
  FLOW_SENSOR_PULSES_PER_LITER,
  WATER_FLOW_PIN
);

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
  bool spray_pesticide;
  bool apply_fertilizer;
  uint16_t irrigation_duration_sec;
} __attribute__((packed)) control_payload_t;

/* ============================
   STATUS PAYLOAD
============================ */

typedef struct {
  bool alive;
  bool valve_state;
  bool spray_state;
  bool fertilizer_state;
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
static_assert(sizeof(farm_packet_t) == 90, "Farm packet layout changed");
static_assert(sizeof(farm_packet_t) <= ESP_NOW_MAX_DATA_LEN, "Farm packet is too large");

/* ============================
   GLOBAL STATE
============================ */

bool valve_state = false;
bool spray_state = false;
bool fertilizer_state = false;
bool valve_timer_active = false;
unsigned long valve_off_time = 0;

unsigned long last_sensor_time = 0;
unsigned long last_flow_read_time = 0;
unsigned long last_status_time = 0;
uint32_t packet_counter = 0;
uint32_t packets_sent = 0;

uint8_t sensor_peers[MAX_SENSOR_PEERS][6];
uint8_t sensor_peer_count = 0;

void send_status();

void count_water_flow_pulse() {
  water_flow_sensor.count();
}

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

  strncpy(destination, source, destination_size - 1);
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

bool mac_equal(
  const uint8_t *left,
  const uint8_t *right
) {
  return memcmp(left, right, 6) == 0;
}

void add_peer_if_needed(const uint8_t *mac) {
  if (esp_now_is_peer_exist(mac)) {
    return;
  }

  esp_now_peer_info_t peer_info = {};
  memcpy(peer_info.peer_addr, mac, 6);
  peer_info.channel = WIFI_CHANNEL;
  peer_info.encrypt = false;

  if (esp_now_add_peer(&peer_info) == ESP_OK) {
    Serial.print("Peer added: ");
    print_mac(mac);
  } else {
    Serial.println("Failed to add peer");
  }
}

void remember_sensor_peer(const uint8_t *mac) {
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

float soil_moisture_percent_from_adc(uint16_t adc_value) {
  if (SOIL_MOISTURE_DRY_ADC == SOIL_MOISTURE_WET_ADC) {
    return 0.0f;
  }

  float percentage =
    100.0f *
    ((float)SOIL_MOISTURE_DRY_ADC - adc_value) /
    ((float)SOIL_MOISTURE_DRY_ADC - SOIL_MOISTURE_WET_ADC);

  return clamp_float(percentage, 0.0f, 100.0f);
}

uint16_t read_soil_moisture_adc() {
  uint32_t total = 0;

  for (uint8_t i = 0; i < SOIL_MOISTURE_SAMPLE_COUNT; i++) {
    total += analogRead(SOIL_MOISTURE_ADC_PIN);
    delay(2);
  }

  return (uint16_t)(total / SOIL_MOISTURE_SAMPLE_COUNT);
}

sensor_readings_t read_sensors() {
  sensor_readings_t reading = {};

  reading.sample_time_ms = millis();
  reading.ambient_temperature_c = NAN;
  reading.ambient_humidity_percent = NAN;
  reading.soil_temperature_c = NAN;

  int ambient_result = ambient_sensor.read();

  if (ambient_result == DHTLIB_OK) {
    float temperature = ambient_sensor.getTemperature();
    float humidity = ambient_sensor.getHumidity();

    if (isfinite(temperature) && isfinite(humidity)) {
      reading.ambient_temperature_c = temperature;
      reading.ambient_humidity_percent = humidity;
      reading.valid_fields |= SENSOR_AMBIENT_VALID;
    }
  } else {
    Serial.printf("AM2301A read failed: %d\n", ambient_result);
  }

  reading.soil_moisture_adc = read_soil_moisture_adc();
  reading.soil_moisture_percent =
    soil_moisture_percent_from_adc(reading.soil_moisture_adc);
  reading.valid_fields |= SENSOR_SOIL_MOISTURE_VALID;

  soil_temperature_sensor.requestTemperatures();
  float soil_temperature =
    soil_temperature_sensor.getTempCByIndex(0);

  if (
    soil_temperature != DEVICE_DISCONNECTED_C &&
    isfinite(soil_temperature)
  ) {
    reading.soil_temperature_c = soil_temperature;
    reading.valid_fields |= SENSOR_SOIL_TEMPERATURE_VALID;
  } else {
    Serial.println("DS18B20 read failed");
  }

  return reading;
}

void print_sensor_readings(const sensor_readings_t &reading) {
  Serial.printf(
    "ambient_temperature_c: %.2f\n",
    reading.ambient_temperature_c
  );
  Serial.printf(
    "ambient_humidity_percent: %.2f\n",
    reading.ambient_humidity_percent
  );
  Serial.printf(
    "soil_moisture_adc: %u\n",
    reading.soil_moisture_adc
  );
  Serial.printf(
    "soil_moisture_percent: %.2f\n",
    reading.soil_moisture_percent
  );
  Serial.printf(
    "soil_temperature_c: %.2f\n",
    reading.soil_temperature_c
  );
}

void set_valve_state(bool enabled) {
  valve_state = enabled;
  digitalWrite(VALVE_PIN, valve_state ? HIGH : LOW);

  if (!valve_state) {
    valve_timer_active = false;
  }
}

void update_timed_outputs() {
  if (
    valve_timer_active &&
    (long)(millis() - valve_off_time) >= 0
  ) {
    set_valve_state(false);
    Serial.println("Timed irrigation cycle complete");
    send_status();
  }
}

/* ============================
   SEND DATA TO MASTER
============================ */

void send_section_sensor_data() {
  farm_packet_t pkt = {};

  WiFi.macAddress(pkt.source_mac);
  memcpy(pkt.destination_mac, master_mac, 6);

  pkt.section_id = SECTION_ID;
  pkt.node_id = 0;
  copy_text(
    pkt.device_client_id,
    sizeof(pkt.device_client_id),
    SECTION_NODE_CLIENT_ID
  );

  pkt.sequence = packet_counter++;
  pkt.type = MSG_SECTION_DATA;
  pkt.section_data.sensors = read_sensors();
  pkt.section_data.water_flow_rate_l_min =
    water_flow_sensor.getFlowRate_m();
  pkt.section_data.total_water_volume_l =
    water_flow_sensor.getVolume();

  esp_err_t result = esp_now_send(
    master_mac,
    (uint8_t*)&pkt,
    sizeof(pkt)
  );

  if (result == ESP_OK) {
    packets_sent++;
    Serial.println("Section sensor packet queued");
    print_sensor_readings(pkt.section_data.sensors);
    Serial.printf(
      "water_flow_rate_l_min: %.3f\n",
      pkt.section_data.water_flow_rate_l_min
    );
    Serial.printf(
      "total_water_volume_l: %.3f\n",
      pkt.section_data.total_water_volume_l
    );
  } else {
    Serial.printf("Failed to queue section sensor packet: %d\n", result);
  }
}

void send_status() {
  farm_packet_t pkt = {};

  WiFi.macAddress(pkt.source_mac);
  memcpy(pkt.destination_mac, master_mac, 6);

  pkt.section_id = SECTION_ID;
  pkt.node_id = 0;
  copy_text(
    pkt.device_client_id,
    sizeof(pkt.device_client_id),
    SECTION_NODE_CLIENT_ID
  );

  pkt.sequence = packet_counter++;
  pkt.type = MSG_STATUS;
  pkt.status.alive = true;
  pkt.status.valve_state = valve_state;
  pkt.status.spray_state = spray_state;
  pkt.status.fertilizer_state = fertilizer_state;
  pkt.status.uptime_ms = millis();
  pkt.status.packets_sent = packets_sent;

  if (
    esp_now_send(master_mac, (uint8_t*)&pkt, sizeof(pkt)) == ESP_OK
  ) {
    packets_sent++;
  }

  Serial.println("Status sent to master");
}

/* ============================
   ROUTING AND CONTROL
============================ */

void handle_sensor_packet(
  farm_packet_t *pkt,
  const uint8_t *sensor_mac
) {
  remember_sensor_peer(sensor_mac);
  pkt->device_client_id[DEVICE_CLIENT_ID_LENGTH - 1] = '\0';

  Serial.printf("Sensor packet from node %d\n", pkt->node_id);
  Serial.print("Sensor client_id: ");
  Serial.println(pkt->device_client_id);
  print_sensor_readings(pkt->sensor_data);

  WiFi.macAddress(pkt->source_mac);
  memcpy(pkt->destination_mac, master_mac, 6);
  pkt->section_id = SECTION_ID;

  if (
    esp_now_send(master_mac, (uint8_t*)pkt, sizeof(*pkt)) == ESP_OK
  ) {
    packets_sent++;
    Serial.println("Forwarded sensor data to master");
  } else {
    Serial.println("Failed to forward sensor data");
  }
}

void handle_control_packet(farm_packet_t *pkt) {
  Serial.println("Control packet received");

  spray_state = pkt->control.spray_pesticide;
  fertilizer_state = pkt->control.apply_fertilizer;
  set_valve_state(pkt->control.irrigate);

  if (
    valve_state &&
    pkt->control.irrigation_duration_sec > 0
  ) {
    valve_timer_active = true;
    valve_off_time =
      millis() +
      ((unsigned long)pkt->control.irrigation_duration_sec * 1000UL);
  }

  Serial.printf("Valve: %s\n", valve_state ? "ON" : "OFF");
  Serial.printf("Spray: %s\n", spray_state ? "ON" : "OFF");
  Serial.printf("Fertilizer: %s\n", fertilizer_state ? "ON" : "OFF");

  if (valve_state) {
    Serial.printf(
      "Irrigation duration: %u sec\n",
      pkt->control.irrigation_duration_sec
    );
  }

  send_status();
}

void route_packet(
  farm_packet_t *pkt,
  const uint8_t *sender_mac
) {
  switch (pkt->type) {
    case MSG_SENSOR_DATA:
      handle_sensor_packet(pkt, sender_mac);
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
   ESPNOW CALLBACKS
============================ */

void on_data_sent(
  const wifi_tx_info_t *tx_info,
  esp_now_send_status_t status
) {
  Serial.print("Send status: ");
  Serial.println(
    status == ESP_NOW_SEND_SUCCESS ? "Success" : "Failed"
  );
}

void on_data_recv(
  const esp_now_recv_info_t *recv_info,
  const uint8_t *incoming_data,
  int len
) {
  if (len != sizeof(farm_packet_t)) {
    Serial.printf(
      "Invalid packet size: %d, expected %u\n",
      len,
      (unsigned int)sizeof(farm_packet_t)
    );
    return;
  }

  farm_packet_t pkt = {};
  memcpy(&pkt, incoming_data, sizeof(pkt));

  Serial.print("Packet from: ");
  print_mac(recv_info->src_addr);

  route_packet(&pkt, recv_info->src_addr);
}

/* ============================
   INITIALIZATION
============================ */

void init_sensors() {
  ambient_sensor.setType(22);

  analogReadResolution(12);
  pinMode(SOIL_MOISTURE_ADC_PIN, INPUT);

  soil_temperature_sensor.begin();
  soil_temperature_sensor.setResolution(10);

  water_flow_sensor.begin(count_water_flow_pulse);
}

void init_espnow() {
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    while (true) {
      delay(1000);
    }
  }

  esp_now_register_send_cb(on_data_sent);
  esp_now_register_recv_cb(on_data_recv);

  add_peer_if_needed(master_mac);
}

void init_gpio() {
  pinMode(VALVE_PIN, OUTPUT);

  digitalWrite(VALVE_PIN, LOW);
}

void setup() {
  Serial.begin(115200);

  init_gpio();
  init_sensors();

  WiFi.mode(WIFI_STA);
  WiFi.setChannel(WIFI_CHANNEL);

  init_espnow();

  Serial.println("Section Node Online");
  Serial.print("Master MAC: ");
  print_mac(master_mac);
}

void loop() {
  unsigned long now = millis();

  update_timed_outputs();

  if (now - last_flow_read_time >= FLOW_READ_INTERVAL_MS) {
    water_flow_sensor.read();
    last_flow_read_time = now;
  }

  if (now - last_sensor_time >= SENSOR_INTERVAL_MS) {
    send_section_sensor_data();
    last_sensor_time = now;
  }

  if (now - last_status_time >= STATUS_INTERVAL_MS) {
    send_status();
    last_status_time = now;
  }
}
