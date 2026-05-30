# Section ESP32 Firmware

This folder contains the firmware for a section controller ESP32. A section node sits between sensor nodes and the master node. It forwards weather readings upward to the master and applies control commands to local output pins for irrigation, pesticide spraying, and fertilizer control.

## Execution Order

1. Flash the master ESP32 from `../master_esp` or at least confirm the master MAC address.

2. Edit `section_esp.ino`:

   - Set `SECTION_ID` for this physical section.
   - Set `master_mac` to the WiFi station MAC address of the master ESP32.
   - Confirm `WIFI_CHANNEL` matches the master and sensor nodes.
   - Confirm `VALVE_PIN`, `SPRAY_PIN`, and `FERTILIZER_PIN` match the wiring.

3. Open `section_esp.ino` in the Arduino IDE, PlatformIO, or another ESP32 Arduino build environment.

4. Flash the sketch to the section ESP32.

5. Power the master and this section node.

6. Flash and power sensor nodes from `../sensor_esp`, using this section node's MAC address as their destination.

## Runtime Flow

1. `setup()` starts serial output, sets WiFi station mode, initializes GPIO outputs, and starts ESP-NOW.
2. Sensor nodes send weather packets to the section node.
3. `handle_weather_packet()` remembers the sensor peer, rewrites the packet source and destination, sets the section ID, and forwards the weather packet, including vapour pressure deficit, to the master.
4. The master sends `MSG_CONTROL_CMD` packets back to this section node.
5. `handle_control_packet()` updates the valve, spray, and fertilizer GPIO pins.
6. `send_status()` periodically reports output state back to the master.

## Files

- `section_esp.ino` - Complete firmware for the section controller. It contains packet definitions, ESP-NOW peer handling, weather forwarding, control command handling, GPIO output control, and periodic status reporting.

## Configuration Notes

- Each physical section should use a unique `SECTION_ID`.
- Update `master_mac` before flashing. The hard-coded value must match the master ESP32 station MAC.
- The section node dynamically remembers sensor peers when they send weather or heartbeat packets.
- Long irrigation commands currently block inside `delay()`, so the node will not process other messages until the valve cycle finishes.
- The packet structure must stay compatible with `master_esp.ino` and `sensor_esp.ino`.
