#include <WiFi.h>
#include <esp_now.h>
#include <math.h>
#include <time.h>
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
#define CONTROL_INTERVAL_MS 30000           // 30 seconds
#define RAINFALL_FORECAST_HORIZON_SEC 3600
#define RAIN_PROBABILITY_BLOCK_THRESHOLD 0.50f
#define RAIN_AMOUNT_BLOCK_THRESHOLD_MM 1.00f
#define MIN_IRRIGATION_DURATION_SEC 60
#define MAX_IRRIGATION_DURATION_SEC 300

#define MAX_SECTIONS 10
#define MAX_SENSOR_NODES 32
#define TENSOR_ARENA_SIZE (48 * 1024)

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
  uint8_t section_id;
  sensor_payload_t data;
} sensor_cache_t;

sensor_cache_t sensor_cache[MAX_SENSOR_NODES];

typedef struct {
  bool valid;
  float rainfall_mm;
  float rain_probability;
} rainfall_forecast_t;

rainfall_forecast_t latest_rainfall_forecast = {
  false,
  0,
  0
};

/* ============================
   AI MODEL STATE
============================ */

const tflite::Model *rainfall_model = nullptr;
tflite::MicroInterpreter *rainfall_interpreter = nullptr;
TfLiteTensor *rainfall_input = nullptr;
TfLiteTensor *rainfall_probability_output = nullptr;
TfLiteTensor *rainfall_amount_output = nullptr;

tflite::MicroMutableOpResolver<2> rainfall_resolver;
alignas(16) uint8_t tensor_arena[TENSOR_ARENA_SIZE];
bool rainfall_model_ready = false;
time_t model_clock_base_epoch = 0;
unsigned long model_clock_started_ms = 0;

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

void add_section_peer(
  uint8_t *mac
);

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

bool get_local_time(
  struct tm *time_info
) {
  if (model_clock_base_epoch <= 0) {
    return false;
  }

  time_t now =
    model_clock_base_epoch +
    (
      millis() -
      model_clock_started_ms
    ) / 1000;

  localtime_r(
    &now,
    time_info
  );

  return true;
}

int compile_month_index(
  const char *month_name
) {
  static const char months[] =
    "JanFebMarAprMayJunJulAugSepOctNovDec";

  const char *month =
    strstr(
      months,
      month_name
    );

  if (!month) {
    return -1;
  }

  return (month - months) / 3;
}

int day_of_year(
  const struct tm &time_info
) {
  return time_info.tm_yday + 1;
}

float cyclical_sin(
  float value,
  float period,
  int harmonic
) {
  return sinf(
    harmonic * 2.0f * PI * value / period
  );
}

float cyclical_cos(
  float value,
  float period,
  int harmonic
) {
  return cosf(
    harmonic * 2.0f * PI * value / period
  );
}

uint16_t month_hour_index(
  int month,
  int hour
) {
  return (month - 1) * 24 + hour;
}

uint16_t lag_month_hour_index(
  const struct tm &time_info,
  int lag_hours
) {
  struct tm lag_time_info =
    time_info;

  lag_time_info.tm_isdst = -1;

  time_t lag_epoch =
    mktime(&lag_time_info) -
    lag_hours * 3600;

  localtime_r(
    &lag_epoch,
    &lag_time_info
  );

  return month_hour_index(
    lag_time_info.tm_mon + 1,
    lag_time_info.tm_hour
  );
}

void init_time() {
  char month_name[4] = {};
  int day = 0;
  int year = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;

  if (
    sscanf(
      __DATE__,
      "%3s %d %d",
      month_name,
      &day,
      &year
    ) != 3 ||
    sscanf(
      __TIME__,
      "%d:%d:%d",
      &hour,
      &minute,
      &second
    ) != 3
  ) {
    Serial.println(
      "Forecast clock unavailable"
    );
    return;
  }

  int month =
    compile_month_index(month_name);

  if (month < 0) {
    Serial.println(
      "Forecast clock month parse failed"
    );
    return;
  }

  struct tm build_time = {};
  build_time.tm_year = year - 1900;
  build_time.tm_mon = month;
  build_time.tm_mday = day;
  build_time.tm_hour = hour;
  build_time.tm_min = minute;
  build_time.tm_sec = second;
  build_time.tm_isdst = -1;

  model_clock_base_epoch =
    mktime(&build_time);

  model_clock_started_ms =
    millis();

  Serial.println(
    "Offline forecast clock ready"
  );
}

void init_rainfall_model() {
  rainfall_model =
    tflite::GetModel(
      hourly_rainfall_forecaster_tflite
    );

  if (
    rainfall_model->version()
    != TFLITE_SCHEMA_VERSION
  ) {
    Serial.println(
      "Rainfall model schema mismatch"
    );
    return;
  }

  if (
    rainfall_resolver.AddFullyConnected()
    != kTfLiteOk
  ) {
    Serial.println(
      "Failed to add FullyConnected op"
    );
    return;
  }

  if (
    rainfall_resolver.AddLogistic()
    != kTfLiteOk
  ) {
    Serial.println(
      "Failed to add Logistic op"
    );
    return;
  }

  static tflite::MicroInterpreter interpreter(
    rainfall_model,
    rainfall_resolver,
    tensor_arena,
    TENSOR_ARENA_SIZE
  );

  rainfall_interpreter =
    &interpreter;

  if (
    rainfall_interpreter->AllocateTensors()
    != kTfLiteOk
  ) {
    Serial.println(
      "Rainfall tensor allocation failed"
    );
    return;
  }

  rainfall_input =
    rainfall_interpreter->input(0);

  rainfall_probability_output =
    rainfall_interpreter->output(0);

  rainfall_amount_output =
    rainfall_interpreter->output(1);

  if (
    rainfall_input->type != kTfLiteFloat32 ||
    rainfall_input->dims->size != 2 ||
    rainfall_input->dims->data[1] !=
      HOURLY_RAINFALL_FEATURE_COUNT
  ) {
    Serial.println(
      "Rainfall model input shape mismatch"
    );
    return;
  }

  rainfall_model_ready = true;

  Serial.println(
    "Rainfall ML model ready"
  );
}

void build_rainfall_features(
  const struct tm &time_info,
  float *features
) {
  int year =
    time_info.tm_year + 1900;

  int month =
    time_info.tm_mon + 1;

  int hour =
    time_info.tm_hour;

  int minute =
    time_info.tm_min;

  int doy =
    day_of_year(time_info);

  int dow =
    time_info.tm_wday == 0 ?
    6 : time_info.tm_wday - 1;

  int dom =
    time_info.tm_mday;

  uint16_t current_month_hour_index =
    month_hour_index(
      month,
      hour
    );

  uint16_t doy_hour_index =
    (doy - 1) * 24 + hour;

  if (
    doy_hour_index >=
    HOURLY_RAINFALL_DOY_HOUR_COUNT
  ) {
    doy_hour_index =
      HOURLY_RAINFALL_DOY_HOUR_COUNT - 1;
  }

  float hour_fraction =
    hour + minute / 60.0f;

  features[0] =
    year - HOURLY_RAINFALL_BASE_YEAR;

  features[1] =
    dow >= 5 ? 1.0f : 0.0f;

  features[2] =
    HOURLY_RAINFALL_MONTH_HOUR_AMOUNT[
      current_month_hour_index
    ];

  features[3] =
    HOURLY_RAINFALL_MONTH_HOUR_PROBABILITY[
      current_month_hour_index
    ];

  features[4] =
    HOURLY_RAINFALL_DOY_HOUR_AMOUNT[
      doy_hour_index
    ];

  features[5] =
    HOURLY_RAINFALL_DOY_HOUR_PROBABILITY[
      doy_hour_index
    ];

  uint16_t lag_1h_index =
    lag_month_hour_index(
      time_info,
      1
    );

  uint16_t lag_3h_index =
    lag_month_hour_index(
      time_info,
      3
    );

  uint16_t lag_6h_index =
    lag_month_hour_index(
      time_info,
      6
    );

  uint16_t lag_24h_index =
    lag_month_hour_index(
      time_info,
      24
    );

  features[6] =
    HOURLY_RAINFALL_LAG_1H_MONTH_AMOUNT[
      lag_1h_index
    ];

  features[7] =
    HOURLY_RAINFALL_LAG_1H_MONTH_PROBABILITY[
      lag_1h_index
    ];

  features[8] =
    HOURLY_RAINFALL_LAG_3H_MONTH_AMOUNT[
      lag_3h_index
    ];

  features[9] =
    HOURLY_RAINFALL_LAG_3H_MONTH_PROBABILITY[
      lag_3h_index
    ];

  features[10] =
    HOURLY_RAINFALL_LAG_6H_MONTH_AMOUNT[
      lag_6h_index
    ];

  features[11] =
    HOURLY_RAINFALL_LAG_6H_MONTH_PROBABILITY[
      lag_6h_index
    ];

  features[12] =
    HOURLY_RAINFALL_LAG_24H_MONTH_AMOUNT[
      lag_24h_index
    ];

  features[13] =
    HOURLY_RAINFALL_LAG_24H_MONTH_PROBABILITY[
      lag_24h_index
    ];

  features[14] =
    cyclical_sin(month, 12.0f, 1);
  features[15] =
    cyclical_cos(month, 12.0f, 1);
  features[16] =
    cyclical_sin(hour_fraction, 24.0f, 1);
  features[17] =
    cyclical_cos(hour_fraction, 24.0f, 1);
  features[18] =
    cyclical_sin(hour_fraction, 24.0f, 2);
  features[19] =
    cyclical_cos(hour_fraction, 24.0f, 2);
  features[20] =
    cyclical_sin(doy, 365.25f, 1);
  features[21] =
    cyclical_cos(doy, 365.25f, 1);
  features[22] =
    cyclical_sin(doy, 365.25f, 2);
  features[23] =
    cyclical_cos(doy, 365.25f, 2);
  features[24] =
    cyclical_sin(dow, 7.0f, 1);
  features[25] =
    cyclical_cos(dow, 7.0f, 1);
  features[26] =
    cyclical_sin(dom, 31.0f, 1);
  features[27] =
    cyclical_cos(dom, 31.0f, 1);
}

bool update_rainfall_forecast() {
  if (!rainfall_model_ready) {
    return false;
  }

  struct tm time_info;

  if (!get_local_time(&time_info)) {
    latest_rainfall_forecast.valid = false;
    return false;
  }

  time_info.tm_isdst = -1;
  time_t forecast_epoch =
    mktime(&time_info) +
    RAINFALL_FORECAST_HORIZON_SEC;

  localtime_r(
    &forecast_epoch,
    &time_info
  );

  Serial.printf(
    "Forecasting rainfall one hour ahead for %04d-%02d-%02d %02d:%02d:%02d\n",
    time_info.tm_year + 1900,
    time_info.tm_mon + 1,
    time_info.tm_mday,
    time_info.tm_hour,
    time_info.tm_min,
    time_info.tm_sec
  );

  float features[
    HOURLY_RAINFALL_FEATURE_COUNT
  ];

  build_rainfall_features(
    time_info,
    features
  );

  for (
    int i = 0;
    i < HOURLY_RAINFALL_FEATURE_COUNT;
    i++
  ) {
    rainfall_input->data.f[i] =
      (
        features[i] -
        HOURLY_RAINFALL_X_MEAN[i]
      ) / HOURLY_RAINFALL_X_STD[i];
  }

  if (
    rainfall_interpreter->Invoke()
    != kTfLiteOk
  ) {
    latest_rainfall_forecast.valid = false;
    Serial.println(
      "Rainfall inference failed"
    );
    return false;
  }

  float probability =
    rainfall_probability_output->data.f[0];

  float amount_scaled =
    rainfall_amount_output->data.f[0];

  float amount_log =
    amount_scaled *
    HOURLY_RAINFALL_Y_AMOUNT_STD +
    HOURLY_RAINFALL_Y_AMOUNT_MEAN;

  latest_rainfall_forecast.rain_probability =
    clamp_float(probability, 0.0f, 1.0f);

  float rainfall_mm =
    expm1f(amount_log);

  latest_rainfall_forecast.rainfall_mm =
    rainfall_mm > 0.0f ? rainfall_mm : 0.0f;

  latest_rainfall_forecast.valid = true;

  Serial.printf(
    "ML rainfall forecast: %.2f mm, probability %.2f\n",
    latest_rainfall_forecast.rainfall_mm,
    latest_rainfall_forecast.rain_probability
  );

  return true;
}

/* ============================
   AI DECISION ENGINE
============================ */

control_payload_t run_ai_model(
  sensor_payload_t sensor
) {
  control_payload_t control;

  control.irrigate = false;
  control.spray_pesticide = false;
  control.apply_fertilizer = false;
  control.irrigation_duration_sec = 0;

  float forecast_probability =
    latest_rainfall_forecast.valid ?
    latest_rainfall_forecast.rain_probability :
    0.0f;

  float forecast_amount_mm =
    latest_rainfall_forecast.valid ?
    latest_rainfall_forecast.rainfall_mm :
    0.0f;

  bool rain_expected =
    forecast_probability >=
      RAIN_PROBABILITY_BLOCK_THRESHOLD ||
    forecast_amount_mm >=
      RAIN_AMOUNT_BLOCK_THRESHOLD_MM;

  // Irrigation logic
  if (
    sensor.moisture < 35 &&
    !rain_expected
  ) {
    control.irrigate = true;

    float moisture_deficit =
      clamp_float(35.0f - sensor.moisture, 0.0f, 35.0f);

    control.irrigation_duration_sec =
      MIN_IRRIGATION_DURATION_SEC +
      (uint16_t)(
        moisture_deficit *
        (
          MAX_IRRIGATION_DURATION_SEC -
          MIN_IRRIGATION_DURATION_SEC
        ) / 35.0f
      );
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
  farm_packet_t *pkt,
  const uint8_t *section_mac
) {
  if (
    pkt->node_id >= MAX_SENSOR_NODES ||
    pkt->section_id >= MAX_SECTIONS
  ) {
    Serial.println(
      "Sensor packet index out of range"
    );
    return;
  }

  memcpy(
    sections[pkt->section_id].mac,
    section_mac,
    6
  );

  sections[pkt->section_id].active = true;

  if (
    !esp_now_is_peer_exist(section_mac)
  ) {
    add_section_peer(
      sections[pkt->section_id].mac
    );
  }

  sensor_cache[pkt->node_id].valid = true;
  sensor_cache[pkt->node_id].section_id =
    pkt->section_id;
  sensor_cache[pkt->node_id].data = pkt->sensor;

  Serial.printf(
    "Sensor packet from Section %d Node %d\n",
    pkt->section_id,
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
  if (
    len != sizeof(farm_packet_t)
  ) {
    Serial.println(
      "Invalid packet size"
    );
    return;
  }

  farm_packet_t pkt;

  memcpy(
    &pkt,
    incoming_data,
    sizeof(pkt)
  );

  switch (pkt.type) {
    case MSG_SENSOR_DATA:
      handle_sensor_packet(
        &pkt,
        recv_info->src_addr
      );
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
        Serial.print(
          "Address received the packet: "
        );

        print_mac(
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
   CONTROL LOOP
============================ */

void process_control_cycle() {
  update_rainfall_forecast();

  for (
    int i = 0;
    i < MAX_SENSOR_NODES;
    i++
  ) {
    if (
      sensor_cache[i].valid
    ) {
      uint8_t section_id =
        sensor_cache[i].section_id;

      if (
        section_id >= MAX_SECTIONS ||
        !sections[section_id].active
      ) {
        continue;
      }

      control_payload_t control =
        run_ai_model(
          sensor_cache[i].data
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
}

/* ============================
   SETUP
============================ */

void setup() {
  Serial.begin(115200);

  init_time();

  init_rainfall_model();

  WiFi.mode(WIFI_STA);

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
    last_control_cycle >
    CONTROL_INTERVAL_MS
  ) {
    process_control_cycle();

    last_control_cycle =
      now;
  }
}
