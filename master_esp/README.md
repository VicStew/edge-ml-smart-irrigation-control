# Master ESP32 Firmware

The master receives physical sensor readings from section ESP32 nodes, publishes telemetry through the SIM800L/ThingsBoard connection, sends irrigation commands back over ESP-NOW, monitors its battery, and controls the tank-fill water pump.

The embedded TensorFlow Lite rainfall model is intentionally disabled and its deployed firmware files have been removed for now. The automatic irrigation algorithm remains active and uses calibrated soil-moisture readings directly.

## FreeRTOS execution model

The master uses both ESP32 cores:

- `farm_task` runs on core 1 at priority 2. It drains a FreeRTOS queue of
  ESP-NOW packets, updates sensor caches, debounces the tank float switches,
  and runs automatic irrigation.
- `cloud_task` runs on core 1 at priority 1. It owns TinyGSM, MQTT, shared
  attribute processing, and telemetry publication. `TINY_GSM_YIELD_MS` is
  set to 1 ms so TinyGSM's long polling operations block briefly and let the
  idle task service the task watchdog.

ESP32 Wi-Fi and ESP-NOW system work remains on core 0. Keeping the blocking
SIM800L task on core 1 prevents modem startup from starving `IDLE0` and the
Wi-Fi stack.

ESP-NOW receive callbacks only validate and enqueue packets, keeping the
ESP32 Wi-Fi task short. Telemetry also uses a FreeRTOS queue, so packet
reception never edits the cloud task's queue storage concurrently.

The master keeps one ThingsBoard Gateway MQTT session open. Downstream
telemetry and attribute updates share this connection, so the firmware no
longer disconnects and changes MQTT identities when sensor traffic arrives.

## Required libraries

- ESP32 Arduino core with ESP-NOW
- TinyGSM
- PubSubClient
- ArduinoJson
- DHTNEW

TensorFlow Lite Micro is no longer required by the firmware.

## Master wiring

| Device | ESP32 pin |
| --- | ---: |
| SIM800L RX (ESP32 receives) | GPIO 16 |
| SIM800L TX (ESP32 transmits) | GPIO 17 |
| Battery voltage-divider output | GPIO 36 |
| AM2301A data | GPIO 27 |
| Pump relay control | GPIO 25 |
| Tank low-level float switch | GPIO 32 |
| Tank high-level float switch | GPIO 33 |

Connect battery positive through R1 (8.2 kOhm) to GPIO36, connect R2 (1 kOhm) from GPIO36 to ground, and join battery and ESP32 grounds. The firmware applies the 9.2 divider ratio to a 16-sample averaged ADC reading. The theoretical source limit is 30.36 V at a 3.3 V ADC input; leave suitable margin for maximum charge voltage, resistor tolerance, and ADC range. Battery percentage is a clamped linear estimate. Configure `MASTER_BATTERY_EMPTY_V`, `MASTER_BATTERY_FULL_V`, `SECTION_BATTERY_EMPTY_V`, `SECTION_BATTERY_FULL_V`, `SENSOR_BATTERY_EMPTY_V`, and `SENSOR_BATTERY_FULL_V` for the batteries actually installed.

GPIO25 only controls a suitable relay module or driver; it must not power the pump directly. The default relay logic is active-high. Change `PUMP_RELAY_ON` and `PUMP_RELAY_OFF` for an active-low relay. Firmware initialization forces the relay off before cloud control starts.

Each two-wire float switch is wired between its GPIO and ground; the firmware enables the ESP32's internal pull-up resistors. `FLOAT_SWITCH_WET_STATE` defaults to `LOW`, meaning the reed contact is expected to close when water reaches the switch. Test each switch before installation because the float orientation can reverse its action; change this constant to `HIGH` if both installed switches present the opposite wet state. Inputs must never be connected directly to the tank or pump supply voltage.

## Setup

1. In ThingsBoard, edit the master device and enable **Is gateway**.
2. Configure the SIM800L pins, APN, ThingsBoard server, and the gateway MQTT
   credential in `MASTER_NODE_CLIENT_ID` in `master_esp.ino`.
3. Keep `WIFI_CHANNEL`, `DEVICE_CLIENT_ID_LENGTH`, message types, and packet
   structs identical in all three sketches.
4. Flash the master, section nodes, and sensor nodes.

The master generates deterministic ThingsBoard device names from ESP-NOW
packet addressing:

- A section controller is named `Section_Node_<section_id>`, such as
  `Section_Node_1`.
- A sensor is named `Sensor_<sensor_id>_Node_<section_id>`, such as
  `Sensor_2_Node_1` for sensor 2 in section 1.

The Gateway API creates these downstream devices if they do not already
exist. To reuse existing ThingsBoard devices, rename them to match this
scheme before starting the gateway.

## Sensor data model

Every sensor reading contains:

- `sample_time_ms`
- `ambient_temperature_c`
- `ambient_humidity_percent`
- `soil_moisture_adc`
- `soil_moisture_percent`
- `soil_temperature_c`
- `valid_fields`

Sensor-node telemetry additionally maps the generic packet voltage fields to:

- `solar_panel_voltage_adc`
- `solar_panel_voltage_v`
- `sunlight_level`
- `sunlight_level_v`
- `battery_voltage_adc`
- `battery_voltage_v`
- `battery_percentage`

Section-node readings contain the same fields plus:

- `battery_voltage_adc`
- `battery_voltage_v`
- `battery_percentage`
- `water_flow_rate_l_min`
- `total_water_volume_l`

The raw ADC values are retained alongside converted values. `valid_fields` bit 3 reports the generic solar/section divider measurement; bit 4 reports the sensor-node battery measurement. These extend the existing ambient, soil-moisture, and soil-temperature validity bits.

The master publishes its own AM2301A reading, battery voltage, `battery_percentage`, low-level tank state, pump state, and uptime every five seconds. Its ambient and pump telemetry keys are:

- `ambient_sensor_valid`
- `ambient_temperature_c`
- `ambient_humidity_percent`
- `pump_state` (the relay state actually applied)
- `requested_pump_state` (the latest shared-attribute command)
- `pump_manual_control`
- `pump_control_mode` (`manual` or `autonomous`)

Temperature and humidity are omitted from a sample when the AM2301A read fails; `ambient_sensor_valid` is still published as `false` so dashboards can identify the failed sample.

## Tank water-level monitoring

Both float inputs use a 50 ms software debounce. The master uses both inputs internally, while its telemetry keys are:

- `tank_low_float_wet`: water has reached the low-mounted switch.
- `tank_low_level`: water is below the low switch.
- `tank_level_state`: `low`, `normal`, `high`, or `sensor_fault`.

The `sensor_fault` state means the high switch reports wet while the low switch reports dry, which is physically inconsistent when both switches are mounted correctly. In autonomous pump mode, the low switch starts filling and the high switch stops it. The high switch also takes precedence and stops the pump for the inconsistent sensor state.

## Automatic irrigation

The master caches readings by both section ID and sensor node ID. Once per `CONTROL_INTERVAL_MS`, it selects the driest valid reading in each discovered section and compares it with `SOIL_MOISTURE_IRRIGATION_THRESHOLD_PERCENT`.

At or below the threshold, irrigation starts for a duration scaled between `MIN_IRRIGATION_DURATION_SEC` and `MAX_IRRIGATION_DURATION_SEC`. Drier soil receives a longer duration. Invalid or missing soil-moisture readings never start an automatic cycle. ThingsBoard manual control continues to override automatic control for the selected section.

## ThingsBoard gateway telemetry and control

The master announces discovered nodes on `v1/gateway/connect`, publishes
their measurements on `v1/gateway/telemetry`, and listens continuously on
`v1/gateway/attributes`. Section status telemetry includes valve motion
state, a `valve_open` flag, uptime, alive state, and packets sent.

Gateway telemetry uses the server-timestamp format, placing each sensor
field directly in the device array object. For example:

```json
{
  "Section_Node_1": [
    {
      "soil_moisture_percent": 42.5,
      "water_flow_rate_l_min": 1.8,
      "valve_state": "open"
    }
  ]
}
```

This stores `soil_moisture_percent`, `water_flow_rate_l_min`, and
`valve_state` as separate ThingsBoard telemetry keys. A `values` wrapper is
only needed when sending an explicit Unix `ts`; the firmware uses the server
receive time because its `sample_time_ms` is ESP32 uptime, not Unix time.

Set the boolean shared attribute `valveState` on a section device such as
`Section_Node_1`:

```json
{
  "valveState": true
}
```

`true` opens the section valve indefinitely and `false` closes it. When a
section is first discovered after boot or an MQTT reconnection, the master
requests the current attribute value before publishing its queued telemetry.
The desired value is stored as a manual override so the automatic
soil-moisture cycle cannot immediately reverse it.

Timed manual control is also supported as a `manual_control` (or
`manualControl`) shared attribute on the individual section device:

```json
{
  "enabled": true,
  "irrigate": true,
  "irrigation_duration_sec": 120
}
```

The section ID comes from the target device name, so it is not included in
the object. The duration begins after the valve finishes opening. Set
`enabled` to `false` to release the override and close the section valve.

## Tank-fill pump control

Set the boolean `manual_control` shared attribute on the master ThingsBoard device—not on a gateway child device—to choose the pump mode. `true` selects manual mode and `false` selects autonomous mode. In manual mode, set the boolean `pump_state` shared attribute to control the relay:

```json
{
  "manual_control": true,
  "pump_state": true
}
```

With `manual_control` set to `false`, `pump_state` is remembered but does not override the float switches: the pump starts when the tank is below the low switch and remains on until water reaches the high switch. The master subscribes to live shared-attribute updates and requests both saved values after every MQTT connection. It boots in manual mode with the pump off as a fail-safe.

The relay command is evaluated by the farm task approximately every 10 ms. The actual applied state is returned as the `pump_state` telemetry key on the next five-second master telemetry sample, allowing the ThingsBoard control workflow to confirm the relay state instead of treating the requested attribute as confirmation.
