# Section ESP32 Firmware

A section node reads its own sensors, measures irrigation water flow, forwards sensor-node readings, and controls a motorized ball valve from commands received from the master over ESP-NOW.

## Sensors and libraries

- AM2301A ambient temperature/humidity sensor using `DHTNEW`
- Analog soil-moisture probe using the ESP32 ADC
- DS18B20 soil-temperature sensor using `OneWire` and `DallasTemperature`
- ZJ-G1 pulse-output water-flow sensor using `FlowSensor`

Install `DHTNEW`, `OneWire`, `DallasTemperature`, and `FlowSensor` before compiling.

## Default wiring

| Device | ESP32 pin |
| --- | ---: |
| Soil-moisture analog output | GPIO 34 |
| ZJ-G1 pulse output | GPIO 32 |
| DS18B20 data | GPIO 25 |
| AM2301A data | GPIO 26 |
| Valve-open relay | GPIO 5 |
| Valve-close relay | GPIO 27 |

Update the pin definitions for the board and relay module being used. The relay outputs default to active-high; change `VALVE_RELAY_ON` and `VALVE_RELAY_OFF` for an active-low module. Do not connect a 5 V flow-sensor output directly to an ESP32 input; use an open-collector pull-up to 3.3 V or suitable level shifting.

## Calibration and configuration

1. Set `SECTION_ID`, `SECTION_NODE_CLIENT_ID`, `master_mac`, and `WIFI_CHANNEL`.
2. Calibrate the soil probe and update `SOIL_MOISTURE_DRY_ADC` and `SOIL_MOISTURE_WET_ADC`.
3. `FLOW_SENSOR_PULSES_PER_LITER` defaults to 60, corresponding to the commonly specified ZJ/YF-G1 relation `F(Hz) = Q(L/min)`. Verify the exact sensor label/datasheet and calibrate it with a known water volume before relying on totals.
4. Confirm that both valve pins drive suitable relays rather than the valve motor directly.
5. Verify the valve's full-stroke time and update `VALVE_TRAVEL_TIME_MS` if it differs from 24 seconds.

## Runtime flow

1. Flow pulses are counted by an interrupt and converted to L/min and accumulated liters once per second.
2. The section reads its AM2301A, soil-moisture probe, and DS18B20 every five seconds, then sends those readings plus flow data to the master.
3. Sensor-node packets are forwarded to the master while preserving the sensor node ID and ThingsBoard client ID.
4. A command to irrigate energizes only the open relay. After 24 seconds, the relay switches off and the requested irrigation-duration timer starts.
5. When the irrigation duration expires, the close relay runs for 24 seconds and then switches off. A close command without a duration starts closing immediately.
6. Valve travel and timed irrigation use `millis()` state transitions, so sensing and ESP-NOW communication continue while the valve moves.
7. Relay switching is break-before-make: both outputs are turned off before either direction is energized, and both are never intentionally energized together.
8. Section status reports `unknown`, `closed`, `opening`, `open`, or `closing`, plus uptime and packet count, every ten seconds.
