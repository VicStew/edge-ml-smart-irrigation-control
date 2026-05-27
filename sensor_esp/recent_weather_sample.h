#ifndef RECENT_WEATHER_SAMPLE_H
#define RECENT_WEATHER_SAMPLE_H

#include <stdint.h>

typedef struct {
  uint32_t sample_time;
  float temperature_2m;
  float relative_humidity_2m;
  float soil_temperature_0_to_7cm;
  float et0_fao_evapotranspiration;
  float shortwave_radiation;
} recent_weather_sample_t;

static const uint8_t RECENT_WEATHER_SAMPLE_COUNT = 24;

static const recent_weather_sample_t RECENT_WEATHER_SAMPLES[RECENT_WEATHER_SAMPLE_COUNT] = {
  {0, 18.5f, 82.0f, 19.1f, 0.000f, 0.0f},
  {3600, 18.2f, 84.0f, 18.9f, 0.000f, 0.0f},
  {7200, 17.9f, 86.0f, 18.7f, 0.000f, 0.0f},
  {10800, 17.6f, 88.0f, 18.5f, 0.000f, 0.0f},
  {14400, 17.4f, 89.0f, 18.3f, 0.000f, 0.0f},
  {18000, 17.2f, 90.0f, 18.2f, 0.000f, 0.0f},
  {21600, 17.8f, 88.0f, 18.4f, 0.010f, 15.0f},
  {25200, 19.0f, 82.0f, 19.0f, 0.040f, 85.0f},
  {28800, 20.8f, 74.0f, 20.1f, 0.110f, 220.0f},
  {32400, 22.3f, 66.0f, 21.4f, 0.180f, 390.0f},
  {36000, 23.7f, 59.0f, 22.8f, 0.240f, 540.0f},
  {39600, 24.9f, 54.0f, 24.0f, 0.290f, 650.0f},
  {43200, 25.6f, 50.0f, 25.0f, 0.320f, 720.0f},
  {46800, 26.1f, 48.0f, 25.7f, 0.330f, 760.0f},
  {50400, 25.8f, 49.0f, 26.0f, 0.310f, 700.0f},
  {54000, 24.9f, 53.0f, 25.7f, 0.260f, 560.0f},
  {57600, 23.4f, 60.0f, 24.8f, 0.190f, 380.0f},
  {61200, 21.8f, 68.0f, 23.6f, 0.110f, 190.0f},
  {64800, 20.4f, 75.0f, 22.5f, 0.050f, 60.0f},
  {68400, 19.5f, 80.0f, 21.5f, 0.010f, 5.0f},
  {72000, 19.0f, 83.0f, 20.7f, 0.000f, 0.0f},
  {75600, 18.7f, 85.0f, 20.1f, 0.000f, 0.0f},
  {79200, 18.4f, 86.0f, 19.6f, 0.000f, 0.0f},
  {82800, 18.1f, 87.0f, 19.2f, 0.000f, 0.0f},
};

#endif  // RECENT_WEATHER_SAMPLE_H
