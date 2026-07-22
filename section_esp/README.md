# Section ESP32 Firmware

A section node reads its own sensors, measures irrigation water flow, forwards sensor-node readings, and applies actuator commands received from the master over ESP-NOW.

## Sensors and libraries

- AM2301A ambient temperature/humidity sensor using `DHTNEW`
- Analog soil-moisture probe using the ESP32 ADC
- DS18B20 soil-temperature sensor using `OneWire` and `DallasTemperature`
- ZJ-G1 pulse-output water-flow sensor using `FlowSensor`

Install `DHTNEW`, `OneWire`, `DallasTemperature`, and `FlowSensor` before compiling.

## Default wiring

| Device | ESP32 pin |
| --- | ---: |
| Soil-moisture analog output | GPIO 0 |
| ZJ-G1 pulse output | GPIO 1 |
| DS18B20 data | GPIO 3 |
| AM2301A data | GPIO 4 |
| Irrigation valve output | GPIO 5 |
| Pesticide spray output | GPIO 6 |
| Fertilizer output | GPIO 7 |

These are ESP32-C3 defaults. Update the pin definitions for the board and driver circuit being used. Do not connect a 5 V flow-sensor output directly to an ESP32 input; use an open-collector pull-up to 3.3 V or suitable level shifting.

## Calibration and configuration

1. Set `SECTION_ID`, `SECTION_NODE_CLIENT_ID`, `master_mac`, and `WIFI_CHANNEL`.
2. Calibrate the soil probe and update `SOIL_MOISTURE_DRY_ADC` and `SOIL_MOISTURE_WET_ADC`.
3. `FLOW_SENSOR_PULSES_PER_LITER` defaults to 60, corresponding to the commonly specified ZJ/YF-G1 relation `F(Hz) = Q(L/min)`. Verify the exact sensor label/datasheet and calibrate it with a known water volume before relying on totals.
4. Confirm the valve, spray, and fertilizer outputs drive suitable relay/MOSFET stages rather than the loads directly.

## Runtime flow

1. Flow pulses are counted by an interrupt and converted to L/min and accumulated liters once per second.
2. The section reads its AM2301A, soil-moisture probe, and DS18B20 every five seconds, then sends those readings plus flow data to the master.
3. Sensor-node packets are forwarded to the master while preserving the sensor node ID and ThingsBoard client ID.
4. Master control packets update the valve, spray, and fertilizer outputs.
5. Timed irrigation is non-blocking; the valve closes when its requested duration expires while communications continue.
6. Section status, uptime, and packet count are sent every ten seconds.
