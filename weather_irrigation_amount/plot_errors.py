import numpy as np
import pandas as pd
import tensorflow as tf
import matplotlib.pyplot as plt
import os

# 1. Load data
df = pd.read_csv('historical_weather_data.csv')
df['time'] = pd.to_datetime(df['time'])
df = df.sort_values('time').reset_index(drop=True)

weather_cols = ['temperature_2m_max', 'temperature_2m_min', 'precipitation_sum', 'et0_fao_evapotranspiration', 'shortwave_radiation_sum', 'soil_moisture_0_to_7cm']

for col in weather_cols:
    for i in range(1, 4):
        df[f'{col}_lag_{i}'] = df[col].shift(i)

df['target_precipitation'] = df['precipitation_sum'].shift(-1)
df['target_et0'] = df['et0_fao_evapotranspiration'].shift(-1)
df.dropna(inplace=True)

features = [f'{col}_lag_{i}' for col in weather_cols for i in range(1, 4)]
X = df[features].values
y_precip = df['target_precipitation'].values
y_et0 = df['target_et0'].values

split_idx = int(len(df) * 0.8)
X_test = X[split_idx:]
y_test_precip = y_precip[split_idx:]
y_test_et0 = y_et0[split_idx:]

# Load scaling
X_mean = np.load('X_mean.npy')
X_std = np.load('X_std.npy')
X_test_scaled = (X_test - X_mean) / X_std

# Convert to float32
X_test_scaled = X_test_scaled.astype(np.float32)

def predict_tflite(model_path, x_data):
    interpreter = tf.lite.Interpreter(model_path=model_path)
    interpreter.allocate_tensors()
    input_details = interpreter.get_input_details()
    output_details = interpreter.get_output_details()
    
    predictions = []
    for i in range(len(x_data)):
        interpreter.set_tensor(input_details[0]['index'], [x_data[i]])
        interpreter.invoke()
        pred = interpreter.get_tensor(output_details[0]['index'])[0][0]
        predictions.append(pred)
    return np.array(predictions)

pred_precip = predict_tflite('precip_model.tflite', X_test_scaled)
pred_et0 = predict_tflite('et0_model.tflite', X_test_scaled)

errors_precip = y_test_precip - pred_precip
errors_et0 = y_test_et0 - pred_et0

fig, axs = plt.subplots(1, 2, figsize=(14, 5))

# Plot Precip errors
axs[0].hist(errors_precip, bins=30, color='#3498db', alpha=0.8, edgecolor='black')
axs[0].set_title('Precipitation Error Distribution (Actual - Predicted)', fontsize=12, pad=10)
axs[0].set_xlabel('Error (mm)', fontsize=10)
axs[0].set_ylabel('Frequency', fontsize=10)
axs[0].grid(axis='y', alpha=0.4)
axs[0].axvline(0, color='red', linestyle='dashed', linewidth=1.5)

# Plot ET0 errors
axs[1].hist(errors_et0, bins=30, color='#2ecc71', alpha=0.8, edgecolor='black')
axs[1].set_title('Evapotranspiration Error Distribution (Actual - Predicted)', fontsize=12, pad=10)
axs[1].set_xlabel('Error (mm)', fontsize=10)
axs[1].set_ylabel('Frequency', fontsize=10)
axs[1].grid(axis='y', alpha=0.4)
axs[1].axvline(0, color='red', linestyle='dashed', linewidth=1.5)

plt.tight_layout()

# Save plot to the artifact directory so we can embed it
save_path = '/home/vector/.gemini/antigravity/brain/c2494def-41f8-465c-92b4-e4e1cb6e1919/error_distribution.png'
plt.savefig(save_path, dpi=300)
print(f"Plot saved to {save_path}")
