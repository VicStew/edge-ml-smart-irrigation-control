# Section ESP32 Firmware

A section node reads its own sensors, measures irrigation water flow, forwards sensor-node readings, and controls a motorized ball valve from commands received from the master over ESP-NOW.

## FreeRTOS execution model

The section node uses three tasks across the ESP32's two cores:

- `valve_task` runs on core 1 at priority 3 and checks valve commands and
  motion deadlines every 10 ms.
- `communication_task` runs on core 0 at priority 2 and routes packets copied
  from the ESP-NOW receive queue.
- `sensor_task` runs on core 0 at priority 1 and performs the slower ambient,
  soil, temperature, and water-flow work.

ESP-NOW callbacks only enqueue received packets. Sensor reads and bursts of
forwarded node traffic therefore cannot block valve state transitions.
Outgoing ESP-NOW calls and shared packet counters are synchronized between
the tasks.

## Sensors and libraries

- AM2301A ambient temperature/humidity sensor using `DHTNEW`
- Analog soil-moisture probe using the ESP32 ADC
- DS18B20 soil-temperature sensor using `OneWire` and `DallasTemperature`
- ZJ-G1 pulse-output water-flow sensor using `FlowSensor`
- Battery voltage input using an 8.2 kOhm/1 kOhm divider

Install `DHTNEW`, `OneWire`, `DallasTemperature`, and `FlowSensor` before compiling.

## Default wiring

| Device | ESP32 pin |
| --- | ---: |
| AM2301A data | GPIO 27 |
| Soil-moisture analog output | GPIO 36 |
| Battery voltage-divider output | GPIO 39 |
| DS18B20 data | GPIO 14 |
| ZJ-G1 pulse output | GPIO 5 |
| Valve-open relay | GPIO 25 |
| Valve-close relay | GPIO 26 |

Update the pin definitions for the board and relay module being used. The relay outputs default to active-high; change `VALVE_RELAY_ON` and `VALVE_RELAY_OFF` for an active-low module. Do not connect a 5 V flow-sensor output directly to an ESP32 input; use an open-collector pull-up to 3.3 V or suitable level shifting.

Connect battery positive through R1 (8.2 kOhm) to GPIO39, connect R2 (1 kOhm) from GPIO39 to ground, and join battery and ESP32 grounds. The 9.2 divider ratio is applied to a 16-sample averaged ADC measurement. The section device publishes `battery_voltage_adc` and `battery_voltage_v` through the master. The theoretical source limit is 30.36 V at a 3.3 V ADC input; leave margin for battery maximum charge voltage, resistor tolerance, and ADC range.

## Calibration and configuration

1. Set `SECTION_ID`, `master_mac`, and `WIFI_CHANNEL`.
   `SECTION_NODE_CLIENT_ID` is retained as a packet-level diagnostic identifier;
   the master no longer uses it to authenticate to ThingsBoard.
2. Calibrate the soil probe and update `SOIL_MOISTURE_DRY_ADC` and `SOIL_MOISTURE_WET_ADC`.
3. `FLOW_SENSOR_PULSES_PER_LITER` defaults to 60, corresponding to the commonly specified ZJ/YF-G1 relation `F(Hz) = Q(L/min)`. Verify the exact sensor label/datasheet and calibrate it with a known water volume before relying on totals.
4. Confirm that both valve pins drive suitable relays rather than the valve motor directly.
5. Verify the valve's full-stroke time and update `VALVE_TRAVEL_TIME_MS` if it differs from 24 seconds.
6. If the divider resistor values change, update `VOLTAGE_DIVIDER_R1_OHMS` and `VOLTAGE_DIVIDER_R2_OHMS`.

## Runtime flow

1. Flow pulses are counted by an interrupt and converted to L/min and accumulated liters once per second.
2. The section reads its AM2301A, soil-moisture probe, DS18B20, and battery voltage every five seconds, then sends those readings plus flow data to the master.
3. Sensor-node packets are forwarded to the master while preserving the sensor node ID and diagnostic client ID.
4. A command to irrigate energizes only the open relay. After 24 seconds, the relay switches off and the requested irrigation-duration timer starts.
5. When the irrigation duration expires, the close relay runs for 24 seconds and then switches off. A close command without a duration starts closing immediately.
6. Valve travel and timed irrigation use `millis()` state transitions, so sensing and ESP-NOW communication continue while the valve moves.
7. Relay switching is break-before-make: both outputs are turned off before either direction is energized, and both are never intentionally energized together.
8. Section status reports `unknown`, `closed`, `opening`, `open`, or `closing`, plus uptime and packet count, every ten seconds.
