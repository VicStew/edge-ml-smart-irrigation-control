# Master ESP32 Firmware

This folder contains the firmware for the master ESP32. The master receives weather readings from section ESP32 nodes, runs the embedded TensorFlow Lite Micro rainfall model, and sends irrigation control commands back to the section nodes over ESP-NOW.

## Execution Order

1. Train or refresh the hourly rainfall model from `../weather_irrigation_hourly`:

   ```sh
   cd ../weather_irrigation_hourly
   python3 fetch_hourly_weather_data.py
   python3 train_hourly_rainfall_model.py
   ```

   The training script copies the deployable model files into this folder.

2. Open `master_esp.ino` in the Arduino IDE, PlatformIO, or another ESP32 Arduino build environment.

3. Install or enable the required firmware libraries:

   - ESP32 Arduino core
   - ESP-NOW support from the ESP32 core
   - TensorFlow Lite Micro headers and runtime compatible with ESP32

4. Confirm `WIFI_CHANNEL` matches the section and sensor nodes.

5. Flash this sketch to the master ESP32.

6. Power the section nodes and sensor nodes. The master discovers section MAC addresses when weather packets arrive through the section nodes.

## Runtime Flow

1. `setup()` initializes serial output, the rainfall model, WiFi station mode, and ESP-NOW.
2. Section ESP32 nodes forward sensor weather packets to the master.
3. `handle_weather_packet()` stores the latest weather reading and registers the sending section as an ESP-NOW peer.
4. Every `CONTROL_INTERVAL_MS`, `process_control_cycle()` evaluates each known sensor node.
5. `run_ai_model()` normalizes weather features, runs the rainfall model, and decides whether irrigation should run.
6. `send_control_packet()` sends valve, pesticide, fertilizer, and irrigation-duration commands to the relevant section node.

## Files

- `master_esp.ino` - Main firmware for the master node. Defines ESP-NOW packet structures, receives weather and status packets, runs rainfall inference, and sends control commands.
- `hourly_rainfall_forecaster.cpp` - Generated C array containing the deployed TFLite model bytes.
- `hourly_rainfall_forecaster.h` - Header exposing the generated TFLite model array to the sketch.
- `hourly_rainfall_preprocessing.h` - Generated preprocessing constants for feature scaling and rainfall output decoding.

## Configuration Notes

- Keep `WIFI_CHANNEL` identical across `master_esp`, `section_esp`, and `sensor_esp`.
- `RAIN_AMOUNT_BLOCK_THRESHOLD_MM` controls how much forecast rain blocks irrigation.
- `MIN_IRRIGATION_DURATION_SEC` and `MAX_IRRIGATION_DURATION_SEC` bound the irrigation time chosen by the decision engine.
- `TENSOR_ARENA_SIZE` may need adjustment if the deployed model changes.
- The packet structure must stay compatible with `section_esp.ino` and `sensor_esp.ino`.
