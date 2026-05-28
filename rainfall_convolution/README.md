# Rainfall Convolution Model

This folder contains the experimental 1D CNN rainfall forecaster. It trains on the hourly weather dataset from `../weather_irrigation_hourly` and uses a 24-hour sliding window of sensor features to predict rainfall amount.

The current deployed master firmware uses the simpler hourly MLP model from `../weather_irrigation_hourly`, not this CNN model. Use this folder when comparing model approaches or preparing a future firmware version that can consume sequence input.

## Execution Order

1. Build or refresh the shared hourly dataset:

   ```sh
   cd ../weather_irrigation_hourly
   python3 fetch_hourly_weather_data.py
   ```

2. Return to this folder:

   ```sh
   cd ../rainfall_convolution
   ```

3. Train and export the CNN model:

   ```sh
   python3 train_cnn_model.py
   ```

4. Optionally regenerate the exploratory notebook:

   ```sh
   python3 create_cnn_rainfall_nb.py
   ```

5. Open `cnn_rainfall_forecasting.ipynb` in Jupyter if you want the notebook workflow and plots.

## Model Flow

1. `train_cnn_model.py` loads `../weather_irrigation_hourly/historical_weather_data_hourly.csv`.
2. It sorts data by time, clips negative rainfall, and drops rows with missing feature or target values.
3. It scales five input features using training-set mean and standard deviation.
4. It creates 24-hour sequences with `TIME_STEPS = 24`.
5. It trains a Conv1D model to predict log-scaled rainfall amount.
6. It exports Keras, TFLite, C-array, scaling, feature-list, and metrics artifacts.

## Files

- `train_cnn_model.py` - Main training/export script for the 24-hour sliding-window CNN model.
- `create_cnn_rainfall_nb.py` - Generates `cnn_rainfall_forecasting.ipynb` from Python code.
- `cnn_rainfall_forecasting.ipynb` - Notebook version of the CNN training workflow with exploration, plots, training, evaluation, and export cells.
- `cnn_rainfall_forecaster.keras` - Saved Keras model.
- `cnn_rainfall_forecaster.tflite` - TensorFlow Lite model.
- `cnn_rainfall_forecaster.cc` - Generated C array containing the TFLite model bytes.
- `cnn_rainfall_forecaster.h` - Header exposing the generated CNN model array.
- `cnn_feature_columns.json` - Ordered list of input feature columns.
- `cnn_X_mean.npy` - Training-set input feature means.
- `cnn_X_std.npy` - Training-set input feature standard deviations.
- `cnn_y_amount_mean.npy` - Mean of the log-scaled rainfall target.
- `cnn_y_amount_std.npy` - Standard deviation of the log-scaled rainfall target.
- `cnn_rainfall_metrics.json` - Evaluation metrics for the exported CNN model.

## Notes

- This model expects input shaped as 24 time steps by 5 features.
- The generated C array is not copied into `master_esp` by this script.
- To deploy this CNN on the ESP32, the master firmware would need sequence-buffer input handling and a TensorFlow Lite Micro op resolver that includes the CNN operations.
