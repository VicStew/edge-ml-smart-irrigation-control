# Edge-AI Weather-Based Smart Irrigation Control System

This repository contains a prototype smart irrigation system that combines ESP32 field nodes, local ESP-NOW communication, and embedded rainfall prediction. The project was developed to show how irrigation decisions can be made close to the farm section being controlled instead of depending on a continuous cloud connection.

The main deployed workflow uses historical Open-Meteo weather data to train a compact hourly rainfall model, converts that model to TensorFlow Lite Micro artifacts, and runs inference on a master ESP32. Sensor nodes send weather readings through section controllers, and the master sends actuator commands back for irrigation, pesticide, and fertilizer outputs.

## Motivation

Manual irrigation and fixed timers often waste water because they cannot react to changing weather, evapotranspiration, or expected rainfall. Many smart irrigation approaches also assume reliable internet connectivity and remote cloud processing, which can be a weakness in rural or semi-rural deployments.

This project explores a lower-cost edge approach:

- Local ESP32 nodes communicate using ESP-NOW, so the field network does not require a Wi-Fi router during operation.
- Rainfall inference runs on the master ESP32 using TensorFlow Lite Micro.
- Section controllers apply actuator commands locally, allowing the design to scale by farm section.
- Open-Meteo history provides a reproducible weather dataset for model development before field sensors are fully calibrated.

## Project Structure

| Path | Purpose |
| --- | --- |
| `rainfall_mlp/` | Main deployed rainfall model pipeline. It fetches or normalizes hourly weather data, trains the compact MLP model, exports Keras and TFLite artifacts, and copies firmware-ready model files into `master_esp/`. |
| `rainfall_convolution/` | Experimental 1D CNN rainfall model using 24-hour weather sequences. It is useful for comparison work, but it is not the model currently deployed by the master firmware. |
| `master_esp/` | Master ESP32 firmware. Receives weather packets, runs rainfall inference, and sends irrigation/control commands to section nodes. |
| `section_esp/` | Section controller firmware. Relays weather packets from sensors to the master and drives local valve, spray, and fertilizer outputs from master commands. |
| `sensor_esp/` | Sensor node firmware and helper script. The current sketch replays generated recent weather samples, then sends weather and heartbeat packets to a section node. |
| `report_artifacts/` | Project report source, bibliography, figures, style files, and generated report outputs. |

Each major folder has its own README with setup notes, execution order, runtime flow, and file-level details.

## Typical Workflow

1. Build or refresh the hourly training dataset:

   ```sh
   cd rainfall_mlp
   python3 fetch_hourly_weather_data.py
   ```

2. Train and export the deployed rainfall model:

   ```sh
   python3 train_hourly_rainfall_model.py
   ```

   This refreshes model artifacts in `rainfall_mlp/` and copies the firmware-ready files into `master_esp/`.

3. Flash the firmware sketches:

   - Flash `master_esp/master_esp.ino` to the master ESP32.
   - Flash `section_esp/section_esp.ino` to each section controller.
   - Flash `sensor_esp/sensor_esp.ino` to sensor nodes after setting the destination section MAC address.

4. Keep `WIFI_CHANNEL` and packet structures aligned across all ESP32 sketches.

## Data And Model Flow

The deployed MLP model uses seven weather inputs:

- `temperature_2m`
- `relative_humidity_2m`
- `vapour_pressure_deficit_kpa`
- `soil_temperature_0_to_7cm`
- `soil_moisture_0_to_7cm`
- `shortwave_radiation`
- `et0_fao_evapotranspiration_mm`

The training pipeline log-scales rainfall amount, saves feature scaling constants, exports a TensorFlow Lite model, and generates C/C++ files for ESP32 deployment. The master firmware normalizes incoming weather readings with the generated constants before running inference.

## Report

The project report lives in `report_artifacts/`. The LaTeX source is `4th_year_project_report.tex`, with bibliography and image assets stored in the same folder.
