# Sensor ESP32 Firmware

This sketch reads physical field sensors and sends their readings to a section ESP32 over ESP-NOW.

## Sensors and libraries

- AM2301A ambient temperature/humidity sensor using `DHTNEW`
- Analog soil-moisture probe using the ESP32 ADC
- DS18B20 soil-temperature sensor using `OneWire` and `DallasTemperature`

Install the `DHTNEW`, `OneWire`, and `DallasTemperature` libraries before compiling.

## Default wiring

| Device | ESP32 pin |
| --- | ---: |
| AM2301A data | GPIO 4 |
| Soil-moisture analog output | GPIO 0 |
| DS18B20 data | GPIO 3 |

The pins are configuration defaults for an ESP32-C3 and can be changed at the top of `sensor_esp.ino`. Add the pull-up resistor required by the AM2301A and a 4.7 kOhm pull-up from the DS18B20 data line to 3.3 V. Keep every input at ESP32-safe voltage levels.

## Configuration

Before flashing:

1. Set `NODE_ID` and `SECTION_ID`. `SENSOR_NODE_CLIENT_ID` is retained as a
   packet-level diagnostic identifier; the master no longer uses it to
   authenticate to ThingsBoard.
2. Set `section_mac` to the section node's Wi-Fi station MAC.
3. Keep `WIFI_CHANNEL` and the packet structures identical across all three sketches.
4. Measure the ADC values for the probe in dry and fully wet reference soil, then update `SOIL_MOISTURE_DRY_ADC` and `SOIL_MOISTURE_WET_ADC`.

The ADC is sampled eight times and averaged. Each packet contains the raw ADC value and the calibrated 0–100% value, so calibration can be checked from telemetry without losing the original measurement.

## Runtime flow

1. `setup()` initializes the three sensors, Wi-Fi station mode, and ESP-NOW.
2. `read_sensors()` reads the AM2301A, averaged soil-moisture ADC, and DS18B20.
3. `send_sensor_packet()` sends the readings and a validity bitmask every five seconds.
4. `send_heartbeat()` reports uptime and packet count every fifteen seconds.

`recent_weather_sample.h`, its CSV, and the fetch helper are retained only as historical test fixtures; the firmware no longer includes or replays them.
