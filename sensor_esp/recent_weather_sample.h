#ifndef RECENT_WEATHER_SAMPLE_H
#define RECENT_WEATHER_SAMPLE_H

#include <stdint.h>

typedef struct {
  uint32_t sample_time;
  float temperature_2m;
  float relative_humidity_2m;
  float soil_temperature_0_to_7cm;
  float soil_moisture_0_to_7cm;
  float et0_fao_evapotranspiration;
  float shortwave_radiation;
} recent_weather_sample_t;

static const uint8_t RECENT_WEATHER_SAMPLE_COUNT = 24;

static const recent_weather_sample_t RECENT_WEATHER_SAMPLES[RECENT_WEATHER_SAMPLE_COUNT] = {
  {0, 20.6f, 66.0f, 22.8f, 0.322f, 0.37f, 595.0f},
  {3600, 19.7f, 67.0f, 22.1f, 0.321f, 0.23f, 351.0f},
  {7200, 18.6f, 74.0f, 21.2f, 0.32f, 0.1f, 147.0f},
  {10800, 16.5f, 86.0f, 20.0f, 0.319f, 0.01f, 13.0f},
  {14400, 16.0f, 88.0f, 19.0f, 0.319f, 0.0f, 0.0f},
  {18000, 15.6f, 89.0f, 18.4f, 0.316f, 0.0f, 0.0f},
  {21600, 15.1f, 91.0f, 17.9f, 0.315f, 0.0f, 0.0f},
  {25200, 15.4f, 89.0f, 17.6f, 0.316f, 0.0f, 0.0f},
  {28800, 14.9f, 93.0f, 17.4f, 0.316f, 0.0f, 0.0f},
  {32400, 14.4f, 96.0f, 17.0f, 0.318f, 0.0f, 0.0f},
  {36000, 14.8f, 97.0f, 16.9f, 0.319f, 0.0f, 0.0f},
  {39600, 15.0f, 97.0f, 17.4f, 0.317f, 0.0f, 0.0f},
  {43200, 14.7f, 98.0f, 17.0f, 0.318f, 0.0f, 0.0f},
  {46800, 14.6f, 98.0f, 16.7f, 0.319f, 0.0f, 0.0f},
  {50400, 14.6f, 98.0f, 16.5f, 0.319f, 0.0f, 0.0f},
  {54000, 14.8f, 98.0f, 16.4f, 0.32f, 0.01f, 5.0f},
  {57600, 15.1f, 97.0f, 16.5f, 0.322f, 0.03f, 45.0f},
  {61200, 16.1f, 93.0f, 16.9f, 0.323f, 0.11f, 204.0f},
  {64800, 16.6f, 90.0f, 17.5f, 0.325f, 0.18f, 336.0f},
  {68400, 17.9f, 80.0f, 18.6f, 0.324f, 0.3f, 537.0f},
  {72000, 18.4f, 77.0f, 19.4f, 0.323f, 0.31f, 516.0f},
  {75600, 19.5f, 75.0f, 20.5f, 0.321f, 0.4f, 668.0f},
  {79200, 19.9f, 70.0f, 21.4f, 0.32f, 0.45f, 756.0f},
  {82800, 19.8f, 73.0f, 21.8f, 0.32f, 0.42f, 732.0f},
};

#endif  // RECENT_WEATHER_SAMPLE_H
