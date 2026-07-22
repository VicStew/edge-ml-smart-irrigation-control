# Master ESP32 Firmware

This folder contains the firmware for the master ESP32. The master receives weather readings from section ESP32 nodes, publishes those readings to ThingsBoard through a SIM800L module, runs the embedded TensorFlow Lite Micro rainfall model, and sends irrigation control commands back to the section nodes over ESP-NOW.

## Execution Order

1. Train or refresh the hourly rainfall model from `../rainfall_mlp`:

   ```sh
   cd ../rainfall_mlp
   python3 fetch_hourly_weather_data.py
   python3 train_hourly_rainfall_model.py
   ```

   The training script copies the deployable model files into this folder.

2. Open `master_esp.ino` in the Arduino IDE, PlatformIO, or another ESP32 Arduino build environment.

3. Install or enable the required firmware libraries:

   - ESP32 Arduino core
   - ESP-NOW support from the ESP32 core
   - TensorFlow Lite Micro headers and runtime compatible with ESP32
   - TinyGSM
   - PubSubClient
   - ArduinoJson

4. Confirm `WIFI_CHANNEL` matches the section and sensor nodes.

5. Update the ThingsBoard and SIM800L settings in `master_esp.ino`:

   - `MASTER_THINGSBOARD_TOKEN`
   - `MASTER_THINGSBOARD_CLIENT_ID`
   - `THINGSBOARD_ROUTES`
   - `THINGSBOARD_SERVER`
   - `GPRS_APN`, `GPRS_USER`, and `GPRS_PASS`
   - `SIM800_RX_PIN`, `SIM800_TX_PIN`, and `SIM800_BAUD`

6. Update each node sketch so its `*_NODE_CLIENT_ID` matches one entry in the master's `THINGSBOARD_ROUTES`.

7. Flash this sketch to the master ESP32.

8. Power the section nodes and sensor nodes. The master discovers section MAC addresses when weather packets arrive through the section nodes.

## Runtime Flow

1. `setup()` initializes serial output, the rainfall model, ThingsBoard MQTT settings, WiFi station mode, ESP-NOW, and the SIM800L modem.
2. Section ESP32 nodes forward sensor weather packets to the master.
3. `handle_weather_packet()` stores the latest weather reading, registers the sending section as an ESP-NOW peer, and queues telemetry for ThingsBoard.
4. `loop()` maintains the GSM/GPRS and master ThingsBoard MQTT connection, publishes queued telemetry to each routed device token, and receives shared-attribute updates on the master device only.
5. Shared attribute updates can enable manual section control. Manual control overrides the automatic AI command for that section until it is disabled.
6. Every `CONTROL_INTERVAL_MS`, `process_control_cycle()` evaluates each known sensor node.
7. `run_ai_model()` normalizes weather features, runs the rainfall model, and decides whether irrigation should run.
8. `send_control_packet()` sends valve, pesticide, fertilizer, and irrigation-duration commands to the relevant section node.

## ThingsBoard Telemetry

The master publishes each received sensor reading to `v1/devices/me/telemetry` using the ThingsBoard route matched by the packet's `device_client_id`. Telemetry keys include:

- `section_id`
- `node_id`
- `device_client_id`
- `sample_time`
- `temperature_2m`
- `relative_humidity_2m`
- `vapour_pressure_deficit_kpa`
- `soil_temperature_0_to_7cm`
- `soil_moisture_0_to_7cm`
- `et0_fao_evapotranspiration`
- `shortwave_radiation`

Section status packets are also published with `valve_state`, `spray_state`, and `fertilizer_state`.

## ThingsBoard Routing

Each sensor and section packet includes a fixed `device_client_id`. The master uses that value to find the matching `THINGSBOARD_ROUTES` entry, then reconnects MQTT with that route's ThingsBoard client ID and token before publishing telemetry. Add one route per ThingsBoard device:

```cpp
const thingsboard_route_t THINGSBOARD_ROUTES[] = {
  {
    "sensor-1",
    "sensor-1-mqtt-client",
    "PUT_SENSOR_1_ACCESS_TOKEN_HERE",
    false
  },
  {
    "section-1",
    "section-1-mqtt-client",
    "PUT_SECTION_1_ACCESS_TOKEN_HERE",
    false
  }
};
```

Only `MASTER_THINGSBOARD_ROUTE` has `subscribe_for_commands` set to `true`. Control commands must be sent to the master device in ThingsBoard; the master then forwards the command to the target section over ESP-NOW.

## ThingsBoard Manual Control

Create a shared attribute named `manual_control` or `manualControl` with a JSON object value:

```json
{
  "section_id": 1,
  "enabled": true,
  "irrigate": true,
  "irrigation_duration_sec": 120,
  "spray_pesticide": false,
  "apply_fertilizer": false
}
```

Set `enabled` to `false` for the section to release manual override and switch the valve off. The master also accepts flat shared attributes using the same field names if your dashboard widget cannot write a JSON object.

## Files

- `master_esp.ino` - Main firmware for the master node. Defines ESP-NOW packet structures, receives weather and status packets, runs rainfall inference, and sends control commands.
- `hourly_rainfall_forecaster.cpp` - Generated C array containing the deployed TFLite model bytes.
- `hourly_rainfall_forecaster.h` - Header exposing the generated TFLite model array to the sketch.
- `hourly_rainfall_preprocessing.h` - Generated preprocessing constants for feature scaling and rainfall output decoding.

## Configuration Notes

- Keep `WIFI_CHANNEL` identical across `master_esp`, `section_esp`, and `sensor_esp`.
- Keep `DEVICE_CLIENT_ID_LENGTH` identical across `master_esp`, `section_esp`, and `sensor_esp`.
- Keep each node's `SENSOR_NODE_CLIENT_ID` or `SECTION_NODE_CLIENT_ID` identical to the matching route in `master_esp.ino`.
- Wire the SIM800L to the master only. The default firmware uses ESP32 `Serial1` on `SIM800_RX_PIN` 16 and `SIM800_TX_PIN` 17.
- `RAIN_AMOUNT_BLOCK_THRESHOLD_MM` controls how much forecast rain blocks irrigation.
- `MIN_IRRIGATION_DURATION_SEC` and `MAX_IRRIGATION_DURATION_SEC` bound the irrigation time chosen by the decision engine.
- `TENSOR_ARENA_SIZE` may need adjustment if the deployed model changes.
- The deployed model expects weather packets to include `vapour_pressure_deficit_kpa` after relative humidity.
- The packet structure must stay compatible with `section_esp.ino` and `sensor_esp.ino`.
