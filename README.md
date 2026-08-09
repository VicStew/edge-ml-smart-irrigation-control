# Edge-AI Weather-Based Smart Irrigation Control System

This repository contains a prototype smart irrigation system that combines ESP32 field nodes, local ESP-NOW communication, physical soil and ambient sensors, section water-flow measurement, and an experimental rainfall-prediction research pipeline. The project was developed to show how irrigation decisions can be made close to the farm section being controlled instead of depending on a continuous cloud connection.

The current firmware reads AM2301A, analog soil-moisture, and DS18B20 sensors. Section nodes add a ZJ-G1 water-flow reading. Sensor nodes send measurements through section controllers, and the master applies a direct soil-moisture irrigation algorithm before sending actuator commands back. Embedded AI inference is disabled for now; the rainfall model folders remain available for research and future integration.

## Motivation

Manual irrigation and fixed timers often waste water because they cannot react to changing weather, evapotranspiration, or expected rainfall. Many smart irrigation approaches also assume reliable internet connectivity and remote cloud processing, which can be a weakness in rural or semi-rural deployments.

This project explores a lower-cost edge approach:

- Local ESP32 nodes communicate using ESP-NOW, so the field network does not require a Wi-Fi router during operation.
- Irrigation decisions run on the master ESP32 using calibrated soil-moisture readings.
- Section controllers apply actuator commands locally, allowing the design to scale by farm section.
- Open-Meteo history remains available as a reproducible dataset for later model development.

## Project Structure

| Path | Purpose |
| --- | --- |
| `rainfall_mlp/` | Experimental hourly rainfall MLP pipeline and generated research artifacts. It is not currently deployed in the master firmware. |
| `rainfall_convolution/` | Experimental 1D CNN rainfall model using 24-hour weather sequences. |
| `master_esp/` | Master ESP32 firmware. Receives sensor packets, publishes telemetry, and sends soil-moisture-based irrigation/control commands to section nodes. |
| `section_esp/` | Section controller firmware. Reads ambient/soil/flow sensors, relays other sensor-node packets, and controls a two-relay motorized ball valve. |
| `sensor_esp/` | Sensor node firmware for AM2301A ambient readings, analog soil moisture, and DS18B20 soil temperature. |
| `report_artifacts/` | Project report source, bibliography, figures, style files, and generated report outputs. |

Each major folder has its own README with setup notes, execution order, runtime flow, and file-level details.

## Typical Workflow

1. Install the Arduino libraries documented in each firmware folder.

2. Configure node IDs, MAC addresses, the master ThingsBoard gateway
   credential, sensor pins, and soil/flow calibration constants. Enable
   **Is gateway** on the master device in ThingsBoard.

3. Flash the firmware sketches:

   - Flash `master_esp/master_esp.ino` to the master ESP32.
   - Flash `section_esp/section_esp.ino` to each section controller.
   - Flash `sensor_esp/sensor_esp.ino` to sensor nodes after setting the destination section MAC address.

4. Keep `WIFI_CHANNEL` and packet structures aligned across all ESP32 sketches.

## Sensor and Control Flow

Sensor and section nodes transmit ambient temperature/humidity, raw and calibrated soil moisture, and soil temperature. Section nodes also transmit flow rate and accumulated water volume. The master caches readings per section and node, selects the driest valid reading per section, and scales a bounded irrigation duration from the measured soil-moisture deficit. Section nodes open and close each motorized valve with interlocked relays and non-blocking 24-second travel sequences. Manual ThingsBoard commands remain available as an override.

## Report

The project report lives in `report_artifacts/`. The LaTeX source is `4th_year_project_report.tex`, with bibliography and image assets stored in the same folder.
