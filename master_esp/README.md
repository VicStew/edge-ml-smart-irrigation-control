# Master ESP32 Firmware

The master receives physical sensor readings from section ESP32 nodes, publishes telemetry through the SIM800L/ThingsBoard connection, and sends irrigation commands back over ESP-NOW.

The embedded TensorFlow Lite rainfall model is intentionally disabled and its deployed firmware files have been removed for now. The automatic irrigation algorithm remains active and uses calibrated soil-moisture readings directly.

## Required libraries

- ESP32 Arduino core with ESP-NOW
- TinyGSM
- PubSubClient
- ArduinoJson

TensorFlow Lite Micro is no longer required by the firmware.

## Setup

1. Configure the SIM800L pins, APN, ThingsBoard server, and master client ID in `master_esp.ino`.
2. Set each sensor and section sketch's `*_NODE_CLIENT_ID` to that device's ThingsBoard MQTT client ID.
3. Keep `WIFI_CHANNEL`, `DEVICE_CLIENT_ID_LENGTH`, message types, and packet structs identical in all three sketches.
4. Flash the master, section nodes, and sensor nodes.

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

## ThingsBoard telemetry

The master uses each packet's `device_client_id` directly as the ThingsBoard MQTT client ID, so adding a node does not require a routing-table entry in the master firmware. Section status telemetry includes the valve motion state, a `valve_open` flag, uptime, alive state, and packets sent.

Manual control uses a shared `manual_control` (or `manualControl`) object:

```json
{
  "section_id": 1,
  "enabled": true,
  "irrigate": true,
  "irrigation_duration_sec": 120
}
```

The duration begins after the valve finishes opening. Set `enabled` to `false` to release the override and send a close command to the section.
