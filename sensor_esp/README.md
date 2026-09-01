# Sensor ESP32 Firmware

This sketch reads physical field sensors and sends their readings to a section ESP32 over ESP-NOW.

## Sensors and libraries

- AM2301A ambient temperature/humidity sensor using `DHTNEW`
- Analog soil-moisture probe using the ESP32 ADC
- DS18B20 soil-temperature sensor using `OneWire` and `DallasTemperature`
- Solar-panel voltage/sunlight-level input using an 8.2 kOhm/1 kOhm divider
- Sensor-node battery voltage input using a second 8.2 kOhm/1 kOhm divider

Install the `DHTNEW`, `OneWire`, and `DallasTemperature` libraries before compiling.

## Default wiring

| Device | ESP32 pin |
| --- | ---: |
| AM2301A data | GPIO 27 |
| Soil-moisture analog output | GPIO 36 |
| Solar voltage-divider output | GPIO 39 |
| Battery voltage-divider output | GPIO 32 |
| DS18B20 data | GPIO 14 |

These defaults target a classic ESP32 with ADC1 pins GPIO36 and GPIO39 and can be changed at the top of `sensor_esp.ino`. Add the pull-up resistor required by the AM2301A and a 4.7 kOhm pull-up from the DS18B20 data line to 3.3 V. Keep every input at ESP32-safe voltage levels.

Connect the solar positive terminal through R1 (8.2 kOhm) to GPIO39, connect R2 (1 kOhm) from GPIO39 to ground, and join the panel and ESP32 grounds. The firmware multiplies the ADC pin voltage by `(R1 + R2) / R2 = 9.2`, averages 16 samples, and reports `solar_panel_voltage_adc`, `solar_panel_voltage_v`, `sunlight_level`, and `sunlight_level_v` to ThingsBoard. Both sunlight-level keys deliberately retain the voltage scale until field measurements are available for a calibrated light percentage.

At a 3.3 V ADC-pin limit this divider corresponds to a theoretical 30.36 V source limit. Leave suitable safety margin for panel open-circuit voltage, resistor tolerance, and ADC range; never allow the divider output to exceed the ESP32 input rating.

For the sensor battery, connect battery positive through another R1 (8.2 kOhm) to GPIO32 and R2 (1 kOhm) from GPIO32 to ground. A 4.2 V battery produces about 0.457 V at the ADC pin, so this ADC1 channel uses 0 dB attenuation for better resolution and remains available while ESP-NOW is active. Add a 0.1 µF ceramic capacitor from GPIO32 to ground, close to the ESP32, for additional hardware noise filtering.

Each battery update averages 32 samples, then applies an exponential low-pass filter and a 20 mV deadband. This suppresses minor ADC jitter without interrupting ESP-NOW. Every five-second packet includes the result as `battery_voltage_adc` and `battery_voltage_v`.

## Configuration

Before flashing:

1. Set `NODE_ID` and `SECTION_ID`. `SENSOR_NODE_CLIENT_ID` is retained as a
   packet-level diagnostic identifier; the master no longer uses it to
   authenticate to ThingsBoard.
2. Set `section_mac` to the section node's Wi-Fi station MAC.
3. Keep `WIFI_CHANNEL` and the packet structures identical across all three sketches.
4. Measure the ADC values for the probe in dry and fully wet reference soil, then update `SOIL_MOISTURE_DRY_ADC` and `SOIL_MOISTURE_WET_ADC`.
5. If the divider resistor values change, update `VOLTAGE_DIVIDER_R1_OHMS` and `VOLTAGE_DIVIDER_R2_OHMS`.
6. Tune `BATTERY_FILTER_ALPHA` and `BATTERY_VOLTAGE_DEADBAND_V` if field testing requires a faster or steadier response.

The soil ADC is sampled eight times and averaged, the solar divider is sampled 16 times, and the battery divider is sampled 32 times before filtering. Each packet retains raw ADC values alongside the converted soil percentage, panel voltage, and battery voltage, so calibration can be checked from telemetry without losing the original measurements.

## Runtime flow

1. `setup()` initializes the three sensors, Wi-Fi station mode, and ESP-NOW.
2. `read_sensors()` reads the AM2301A, averaged soil-moisture ADC, DS18B20, divided solar-panel voltage, and filtered battery voltage.
3. `send_sensor_packet()` sends the readings and a validity bitmask every five seconds.
4. `send_heartbeat()` reports uptime and packet count every fifteen seconds.

`recent_weather_sample.h`, its CSV, and the fetch helper are retained only as historical test fixtures; the firmware no longer includes or replays them.
