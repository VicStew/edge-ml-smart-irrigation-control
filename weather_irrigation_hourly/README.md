# Hourly Weather Irrigation Model

This folder contains the main rainfall model training pipeline used by the master ESP32 firmware. It downloads hourly weather history, trains a small TensorFlow model from instantaneous weather sensor features, exports TFLite artifacts, and copies the firmware-ready model files into `../master_esp`.

## Execution Order

1. Fetch or normalize hourly weather history:

   ```sh
   python3 fetch_hourly_weather_data.py
   ```

   This writes `historical_weather_data_hourly.csv`.

   If API access is unavailable, export a CSV from Open-Meteo and run:

   ```sh
   python3 fetch_hourly_weather_data.py --source-export path/to/open_meteo_export.csv
   ```

2. Train and export the rainfall model:

   ```sh
   python3 train_hourly_rainfall_model.py
   ```

   This writes model artifacts in this folder and copies deployable C/C++ files into `../master_esp`.

3. Optionally regenerate the exploratory notebook:

   ```sh
   python3 create_hourly_rainfall_nb.py
   ```

4. Flash the master ESP32 from `../master_esp` after training has refreshed:

   - `../master_esp/hourly_rainfall_forecaster.cpp`
   - `../master_esp/hourly_rainfall_forecaster.h`
   - `../master_esp/hourly_rainfall_preprocessing.h`

## Data And Model Flow

1. `fetch_hourly_weather_data.py` downloads Open-Meteo archive data for the configured farm location, normalizes column names, and derives vapour pressure deficit from air temperature and relative humidity.
2. `train_hourly_rainfall_model.py` loads `historical_weather_data_hourly.csv`.
3. The script chronologically splits the data into training and test sets.
4. It trains a small dense neural network using seven features:

   - `temperature_2m`
   - `relative_humidity_2m`
   - `vapour_pressure_deficit_kpa`
   - `soil_temperature_0_to_7cm`
   - `soil_moisture_0_to_7cm`
   - `shortwave_radiation`
   - `et0_fao_evapotranspiration_mm`

5. It log-scales the rainfall target, saves scaling constants, exports Keras and TFLite models, and converts the TFLite file into a C array for ESP32 firmware.

## Files

- `fetch_hourly_weather_data.py` - Fetches historical hourly weather data from Open-Meteo or normalizes a local Open-Meteo export.
- `historical_weather_data_hourly.csv` - Normalized hourly training dataset.
- `train_hourly_rainfall_model.py` - Main training/export script for the deployed hourly rainfall model.
- `create_hourly_rainfall_nb.py` - Generates `hourly_rainfall_forecasting.ipynb` from Python code.
- `hourly_rainfall_forecasting.ipynb` - Notebook version of the training workflow with exploration, plots, training, evaluation, and export cells.
- `hourly_rainfall_forecaster.keras` - Saved Keras model.
- `hourly_rainfall_forecaster.tflite` - TensorFlow Lite model used for firmware conversion.
- `hourly_rainfall_forecaster.h` - Header generated for the local model export.
- `hourly_feature_columns.json` - Ordered list of model input features.
- `hourly_X_mean.npy` - Training-set input feature means.
- `hourly_X_std.npy` - Training-set input feature standard deviations.
- `hourly_y_amount_mean.npy` - Mean of the log-scaled rainfall target.
- `hourly_y_amount_std.npy` - Standard deviation of the log-scaled rainfall target.
- `hourly_rainfall_metrics.json` - Evaluation metrics for the exported model.

## Outputs Used By Firmware

`train_hourly_rainfall_model.py` writes the firmware-ready files directly into `../master_esp`:

- `hourly_rainfall_forecaster.cpp`
- `hourly_rainfall_forecaster.h`
- `hourly_rainfall_preprocessing.h`

These are the files included by `../master_esp/master_esp.ino`.

## Notes

- Network access is required when fetching data directly from Open-Meteo.
- The date range and location are configured near the top of `fetch_hourly_weather_data.py`.
- Keep `hourly_feature_columns.json` and the preprocessing arrays aligned with the model that is deployed to the master ESP32.
