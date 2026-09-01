#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_mac.h>
#include <math.h>
#include <string.h>

#include <dhtnew.h>
#include <OneWire.h>
#include <DallasTemperature.h>

/* ============================
   CONFIGURATION
============================ */

#define WIFI_CHANNEL 4

#define NODE_ID 1
#define SECTION_ID 1

#define AM2301A_PIN 27
#define SOIL_MOISTURE_ADC_PIN 36
#define SOLAR_VOLTAGE_ADC_PIN 39
#define DS18B20_PIN 14

#define SOIL_MOISTURE_DRY_ADC 4095
#define SOIL_MOISTURE_WET_ADC 0
#define SOIL_MOISTURE_SAMPLE_COUNT 8

#define VOLTAGE_DIVIDER_R1_OHMS 8200.0f
#define VOLTAGE_DIVIDER_R2_OHMS 1000.0f
#define VOLTAGE_SAMPLE_COUNT 16

#define SEND_INTERVAL_MS 5000
#define HEARTBEAT_INTERVAL_MS 15000
#define DEVICE_CLIENT_ID_LENGTH 40

const char SENSOR_NODE_CLIENT_ID[] = "0trhe202q8u4wxegyzkf";

/* ============================
   SECTION NODE MAC
============================ */

uint8_t section_mac[6] = {
  0x28, 0x05, 0xA5, 0x2B, 0xF3, 0x0C
};

/* ============================
   SENSORS
============================ */

DHTNEW ambient_sensor(AM2301A_PIN);
OneWire  soil_temperature_bus(DS18B20_PIN);
DallasTemperature soil_temperature_sensor(&soil_temperature_bus);

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
  SENSOR_SOIL_TEMPERATURE_VALID = 1 << 2,
  SENSOR_MONITORED_VOLTAGE_VALID = 1 << 3
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
  uint16_t monitored_voltage_adc;
  float monitored_voltage_v;
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

static_assert(sizeof(sensor_readings_t) == 29, "Sensor payload layout changed");
static_assert(sizeof(section_readings_t) == 37, "Section payload layout changed");
static_assert(sizeof(control_payload_t) == 3, "Control payload layout changed");
static_assert(sizeof(status_payload_t) == 10, "Status payload layout changed");
static_assert(sizeof(farm_packet_t) == 96, "Farm packet layout changed");
static_assert(sizeof(farm_packet_t) <= ESP_NOW_MAX_DATA_LEN, "Farm packet is too large");

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

void read_monitored_voltage(
  uint16_t *adc_value,
  float *voltage_v
) {
  uint32_t raw_total = 0;
  uint32_t millivolt_total = 0;

  for (uint8_t i = 0; i < VOLTAGE_SAMPLE_COUNT; i++) {
    raw_total += analogRead(SOLAR_VOLTAGE_ADC_PIN);
    millivolt_total += analogReadMilliVolts(SOLAR_VOLTAGE_ADC_PIN);
    delay(2);
  }

  *adc_value = (uint16_t)(raw_total / VOLTAGE_SAMPLE_COUNT);

  float adc_voltage_v =
    ((float)millivolt_total / VOLTAGE_SAMPLE_COUNT) / 1000.0f;
  float divider_ratio =
    (VOLTAGE_DIVIDER_R1_OHMS + VOLTAGE_DIVIDER_R2_OHMS) /
    VOLTAGE_DIVIDER_R2_OHMS;

  *voltage_v = adc_voltage_v * divider_ratio;
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

  read_monitored_voltage(
    &reading.monitored_voltage_adc,
    &reading.monitored_voltage_v
  );
  reading.valid_fields |= SENSOR_MONITORED_VOLTAGE_VALID;

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
  Serial.printf(
    "solar_panel_voltage_adc: %u\n",
    reading.monitored_voltage_adc
  );
  Serial.printf(
    "solar_panel_voltage_v (sunlight level): %.3f\n",
    reading.monitored_voltage_v
  );
}

/* ============================
   SEND SENSOR PACKET
============================ */

void send_sensor_packet() {
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
  pkt.type = MSG_SENSOR_DATA;
  pkt.sensor_data = read_sensors();

  esp_err_t result = esp_now_send(
    section_mac,
    (uint8_t*)&pkt,
    sizeof(pkt)
  );

  if (result == ESP_OK) {
    packets_sent++;
    Serial.println("Sensor packet queued");
    print_sensor_readings(pkt.sensor_data);
  } else {
    Serial.printf("Failed to queue sensor packet: %d\n", result);
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
  pkt.status.uptime_ms = millis();
  pkt.status.packets_sent = packets_sent;

  esp_now_send(section_mac, (uint8_t*)&pkt, sizeof(pkt));
  Serial.println("Heartbeat sent");
}

/* ============================
   RECEIVE ROUTER
============================ */

void route_packet(farm_packet_t *pkt) {
  switch (pkt->type) {
    case MSG_CONTROL_CMD:
      Serial.printf(
        "Control command received: irrigate=%s duration=%u sec\n",
        pkt->control.irrigate ? "true" : "false",
        pkt->control.irrigation_duration_sec
      );
      break;

    case MSG_ACK:
      Serial.printf("ACK received seq=%lu\n", pkt->sequence);
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

  Serial.print("Packet received from: ");
  print_mac(recv_info->src_addr);

  route_packet(&pkt);
}

/* ============================
   INITIALIZATION
============================ */

void init_sensors() {
  ambient_sensor.setType(22);

  analogReadResolution(12);
  pinMode(SOIL_MOISTURE_ADC_PIN, INPUT);
  pinMode(SOLAR_VOLTAGE_ADC_PIN, INPUT);
  analogSetPinAttenuation(SOLAR_VOLTAGE_ADC_PIN, ADC_11db);

  soil_temperature_sensor.begin();
  soil_temperature_sensor.setResolution(10);
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

  esp_now_peer_info_t peer_info = {};
  memcpy(peer_info.peer_addr, section_mac, 6);
  peer_info.channel = WIFI_CHANNEL;
  peer_info.encrypt = false;

  if (esp_now_add_peer(&peer_info) != ESP_OK) {
    Serial.println("Failed to add section peer");
  }
}

void setup() {
  Serial.begin(115200);

  init_sensors();

  WiFi.mode(WIFI_STA);
  WiFi.setChannel(WIFI_CHANNEL);

  init_espnow();

  Serial.println("Sensor Node Online");
  Serial.print("Section MAC: ");
  print_mac(section_mac);
}

void loop() {
  unsigned long now = millis();

  if (now - last_send >= SEND_INTERVAL_MS) {
    send_sensor_packet();
    last_send = now;
  }

  if (now - last_heartbeat >= HEARTBEAT_INTERVAL_MS) {
    send_heartbeat();
    last_heartbeat = now;
  }
}
