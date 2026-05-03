from pathlib import Path

import nbformat as nbf

BASE_DIR = Path(__file__).resolve().parent

nb = nbf.v4.new_notebook()

nb['cells'] = [
    nbf.v4.new_markdown_cell("# Unified Multi-Output Weather Model (ESP32)\n\nThis notebook trains a single Dense (MLP) model to simultaneously predict precipitation and evapotranspiration using the expanded weather dataset."),
    
    nbf.v4.new_markdown_cell("## 1. Import Libraries"),
    nbf.v4.new_code_cell("import pandas as pd\nimport numpy as np\nimport tensorflow as tf\nfrom tensorflow.keras.models import Sequential\nfrom tensorflow.keras.layers import Dense, Input, Flatten\nfrom sklearn.metrics import mean_absolute_error, r2_score\nimport warnings\nwarnings.filterwarnings('ignore')"),

    nbf.v4.new_markdown_cell("## 2. Load Dataset"),
    nbf.v4.new_code_cell("df = pd.read_csv('historical_weather_data.csv')\ndf['time'] = pd.to_datetime(df['time'])\ndf = df.sort_values('time').reset_index(drop=True)\nprint(f'Loaded {len(df)} days of historical weather data.')"),

    nbf.v4.new_markdown_cell("## 3. Feature Engineering"),
    nbf.v4.new_code_cell("""weather_cols = ['temperature_2m_max', 'temperature_2m_min', 'precipitation_sum', 'et0_fao_evapotranspiration', 'shortwave_radiation_sum', 'soil_moisture_0_to_7cm', 'relative_humidity_2m', 'vapor_pressure_deficit', 'wind_speed_10m']

for col in weather_cols:
    for i in range(1, 4):
        df[f'{col}_lag_{i}'] = df[col].shift(i)

df['target_precipitation'] = df['precipitation'].shift(-1)
df['target_evapotranspiration'] = df['evapotranspiration'].shift(-1)
df.dropna(inplace=True)
df.reset_index(drop=True, inplace=True)

X_3d = np.zeros((len(df), 3, len(weather_cols)))
for i, col in enumerate(weather_cols):
    X_3d[:, 0, i] = df[f'{col}_lag_3']
    X_3d[:, 1, i] = df[f'{col}_lag_2']
    X_3d[:, 2, i] = df[f'{col}_lag_1']

# Unified multi-output target array: shape (N, 2)
y = np.column_stack((df['target_precipitation'].values, df['target_evapotranspiration'].values))

print(f'Input Shape: {X_3d.shape}')
print(f'Target Shape: {y.shape}')
"""),

    nbf.v4.new_markdown_cell("## 4. Train/Test Split & Normalization"),
    nbf.v4.new_code_cell("""split_idx = int(len(df) * 0.8)

X_train, X_test = X_3d[:split_idx], X_3d[split_idx:]
y_train, y_test = y[:split_idx], y[split_idx:]

X_mean = X_train.mean(axis=0)
X_std = X_train.std(axis=0)
X_std[X_std == 0] = 1e-6

X_train_scaled = (X_train - X_mean) / X_std
X_test_scaled = (X_test - X_mean) / X_std

np.save('X_mean_unified.npy', X_mean)
np.save('X_std_unified.npy', X_std)
print(f'Training on {len(X_train)} days, Testing on {len(X_test)} days.')
"""),

    nbf.v4.new_markdown_cell("## 5. Unified Model Architecture"),
    nbf.v4.new_code_cell("""model = Sequential([
    Input(shape=(3, len(weather_cols))),
    Flatten(),
    Dense(32, activation='relu'),
    Dense(16, activation='relu'),
    Dense(2) # 2 Outputs: [Precipitation, ET0]
])

model.compile(optimizer='adam', loss='mse')
print(model.summary())
"""),

    nbf.v4.new_markdown_cell("## 6. Train Model"),
    nbf.v4.new_code_cell("""print("Training Unified Model...")
model.fit(X_train_scaled, y_train, epochs=60, batch_size=16, verbose=0, validation_data=(X_test_scaled, y_test))
print("Training complete.")
"""),

    nbf.v4.new_markdown_cell("## 7. Performance Evaluation"),
    nbf.v4.new_code_cell("""preds = model.predict(X_test_scaled, verbose=0)

# Slice outputs
pred_precip = preds[:, 0]
pred_evapotranspiration = preds[:, 1]
y_test_precip = y_test[:, 0]
y_test_evapotranspiration = y_test[:, 1]

mae_p = mean_absolute_error(y_test_precip, pred_precip)
r2_p = r2_score(y_test_precip, pred_precip)

mae_e = mean_absolute_error(y_test_evapotranspiration, pred_evapotranspiration)
r2_e = r2_score(y_test_evapotranspiration, pred_evapotranspiration)

print("========== PRECIPITATION (Unified) ==========")
print(f"MAE: {mae_p:.3f} mm  | R²: {r2_p:.3f}")
print("\\n========== EVAPOTRANSPIRATION (Unified) ==========")
print(f"MAE: {mae_e:.3f} mm  | R²: {r2_e:.3f}")
"""),

    nbf.v4.new_markdown_cell("## 8. Export to TFLite"),
    nbf.v4.new_code_cell("""converter = tf.lite.TFLiteConverter.from_keras_model(model)
converter.optimizations = [tf.lite.Optimize.DEFAULT]
tflite_model = converter.convert()

with open('unified_weather_model.tflite', 'wb') as f:
    f.write(tflite_model)
print(f'Saved unified_weather_model.tflite ({len(tflite_model)} bytes)')
"""),

    nbf.v4.new_markdown_cell("## 9. Generate C Array"),
    nbf.v4.new_code_cell("""def convert_tflite_to_c_array(tflite_path, c_file_path, array_name):
    with open(tflite_path, 'rb') as f:
        tflite_content = f.read()

    hex_array = [f'0x{b:02x}' for b in tflite_content]
    
    with open(c_file_path, 'w') as f:
        f.write(f'#include "{array_name}.h"\\n\\n')
        f.write(f'const unsigned char {array_name}[] = {{\\n')
        for i in range(0, len(hex_array), 12):
            f.write('  ' + ', '.join(hex_array[i:i+12]) + ',\\n')
        f.write('};\\n\\n')
        f.write(f'const int {array_name}_len = {len(hex_array)};\\n')

    h_file_path = c_file_path.replace('.cc', '.h')
    with open(h_file_path, 'w') as f:
        f.write(f'#ifndef {array_name.upper()}_H\\n')
        f.write(f'#define {array_name.upper()}_H\\n\\n')
        f.write(f'extern const unsigned char {array_name}[];\\n')
        f.write(f'extern const int {array_name}_len;\\n\\n')
        f.write(f'#endif // {array_name.upper()}_H\\n')
        
    print(f'Generated {c_file_path} and {h_file_path}')

convert_tflite_to_c_array('unified_weather_model.tflite', 'unified_weather_model.cc', 'unified_weather_model_tflite')
""")
]

with open(BASE_DIR / 'weather_forecasting_model.ipynb', 'w') as f:
    nbf.write(nb, f)
