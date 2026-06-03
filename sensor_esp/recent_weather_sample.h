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
  {0, 19.3f, 71.0f, 19.6f, 0.358f, 0.39f, 644.0f},
  {3600, 19.8f, 67.0f, 20.2f, 0.355f, 0.37f, 598.0f},
  {7200, 20.0f, 65.0f, 20.9f, 0.358f, 0.43f, 723.0f},
  {10800, 19.3f, 66.0f, 20.5f, 0.355f, 0.25f, 356.0f},
  {14400, 18.9f, 69.0f, 20.2f, 0.354f, 0.21f, 316.0f},
  {18000, 17.8f, 76.0f, 19.5f, 0.352f, 0.1f, 146.0f},
  {21600, 15.9f, 84.0f, 18.5f, 0.351f, 0.01f, 15.0f},
  {25200, 15.4f, 84.0f, 17.6f, 0.35f, 0.0f, 0.0f},
  {28800, 15.2f, 85.0f, 17.2f, 0.349f, 0.0f, 0.0f},
  {32400, 15.1f, 83.0f, 16.8f, 0.349f, 0.0f, 0.0f},
  {36000, 14.9f, 81.0f, 16.5f, 0.348f, 0.0f, 0.0f},
  {39600, 15.4f, 82.0f, 16.5f, 0.347f, 0.01f, 0.0f},
  {43200, 14.1f, 96.0f, 16.3f, 0.347f, 0.0f, 0.0f},
  {46800, 14.7f, 93.0f, 16.3f, 0.348f, 0.0f, 0.0f},
  {50400, 14.1f, 99.0f, 16.2f, 0.344f, 0.0f, 0.0f},
  {54000, 14.1f, 99.0f, 16.0f, 0.344f, 0.0f, 0.0f},
  {57600, 14.0f, 100.0f, 15.8f, 0.344f, 0.0f, 0.0f},
  {61200, 14.0f, 100.0f, 15.6f, 0.344f, 0.0f, 0.0f},
  {64800, 14.1f, 99.0f, 15.5f, 0.343f, 0.0f, 8.0f},
  {68400, 14.9f, 94.0f, 15.9f, 0.343f, 0.06f, 113.0f},
  {72000, 16.1f, 88.0f, 16.5f, 0.344f, 0.15f, 284.0f},
  {75600, 17.0f, 85.0f, 17.2f, 0.346f, 0.21f, 376.0f},
  {79200, 18.1f, 80.0f, 18.1f, 0.348f, 0.3f, 524.0f},
  {82800, 19.0f, 75.0f, 19.1f, 0.348f, 0.37f, 626.0f},
};

#endif  // RECENT_WEATHER_SAMPLE_H
