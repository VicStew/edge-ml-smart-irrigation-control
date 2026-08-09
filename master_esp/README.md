# Master ESP32 Firmware

The master receives physical sensor readings from section ESP32 nodes, publishes telemetry through the SIM800L/ThingsBoard connection, and sends irrigation commands back over ESP-NOW.

The embedded TensorFlow Lite rainfall model is intentionally disabled and its deployed firmware files have been removed for now. The automatic irrigation algorithm remains active and uses calibrated soil-moisture readings directly.

## FreeRTOS execution model

The master uses both ESP32 cores:

- `farm_task` runs on core 1 at priority 2. It drains a FreeRTOS queue of
  ESP-NOW packets, updates sensor caches, and runs automatic irrigation.
- `cloud_task` runs on core 0 at priority 1. It owns TinyGSM, MQTT, shared
  attribute processing, and telemetry publication.

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

TensorFlow Lite Micro is no longer required by the firmware.

## Setup

1. In ThingsBoard, edit the master device and enable **Is gateway**.
2. Configure the SIM800L pins, APN, ThingsBoard server, and the gateway MQTT
   credential in `MASTER_NODE_CLIENT_ID` in `master_esp.ino`.
3. Keep `WIFI_CHANNEL`, `DEVICE_CLIENT_ID_LENGTH`, message types, and packet
   structs identical in all three sketches.
4. Flash the master, section nodes, and sensor nodes.

The master generates deterministic ThingsBoard device names from ESP-NOW
packet addressing:

- A section controller is named `Section_<section_id>`, such as `Section_1`.
- A sensor is named `Section_<section_id>_Sensor_<node_id>`, such as
  `Section_1_Sensor_2`.

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

Section-node readings contain the same fields plus:

- `water_flow_rate_l_min`
- `total_water_volume_l`

The raw soil ADC value is retained alongside the calibrated percentage. `valid_fields` reports whether the ambient, soil-moisture, and soil-temperature readings succeeded.

## Automatic irrigation

The master caches readings by both section ID and sensor node ID. Once per `CONTROL_INTERVAL_MS`, it selects the driest valid reading in each discovered section and compares it with `SOIL_MOISTURE_IRRIGATION_THRESHOLD_PERCENT`.

At or below the threshold, irrigation starts for a duration scaled between `MIN_IRRIGATION_DURATION_SEC` and `MAX_IRRIGATION_DURATION_SEC`. Drier soil receives a longer duration. Invalid or missing soil-moisture readings never start an automatic cycle. ThingsBoard manual control continues to override automatic control for the selected section.

## ThingsBoard gateway telemetry and control

The master announces discovered nodes on `v1/gateway/connect`, publishes
their measurements on `v1/gateway/telemetry`, and listens continuously on
`v1/gateway/attributes`. Section status telemetry includes valve motion
state, a `valve_open` flag, uptime, alive state, and packets sent.

Set the boolean shared attribute `valveState` on a section device such as
`Section_1`:

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
