import nbformat as nbf

nb = nbf.v4.new_notebook()

nb['cells'] = [
    nbf.v4.new_markdown_cell("# Weather Forecasting: GRU vs. Dense Architecture\n\nThis notebook evaluates a Gated Recurrent Unit (GRU) against our existing Dense (MLP) architecture to determine the optimal structure for ESP32 TFLite Micro deployment."),
    
    nbf.v4.new_markdown_cell("## 1. Import Libraries"),
    nbf.v4.new_code_cell("import pandas as pd\nimport numpy as np\nimport tensorflow as tf\nfrom tensorflow.keras.models import Sequential\nfrom tensorflow.keras.layers import Dense, Input, GRU, Flatten\nfrom sklearn.metrics import mean_absolute_error, r2_score\nimport warnings\nwarnings.filterwarnings('ignore')"),

    nbf.v4.new_markdown_cell("## 2. Load Dataset"),
    nbf.v4.new_code_cell("df = pd.read_csv('historical_weather_data.csv')\ndf['time'] = pd.to_datetime(df['time'])\ndf = df.sort_values('time').reset_index(drop=True)\nprint(f'Loaded {len(df)} days of historical weather data.')"),

    nbf.v4.new_markdown_cell("## 3. Feature Engineering (3D Time-Series Lags)\nWe explicitly format the input data into a 3D tensor of shape `(Samples, Timesteps, Features)`."),
    nbf.v4.new_code_cell("""weather_cols = ['temperature_2m_max', 'temperature_2m_min', 'precipitation_sum', 'et0_fao_evapotranspiration', 'shortwave_radiation_sum', 'soil_moisture_0_to_7cm']

# Create flat lags first to shift data
for col in weather_cols:
    for i in range(1, 4):
        df[f'{col}_lag_{i}'] = df[col].shift(i)

# Create Target variables (predicting TOMORROW)
df['target_precipitation'] = df['precipitation_sum'].shift(-1)
df['target_et0'] = df['et0_fao_evapotranspiration'].shift(-1)
df.dropna(inplace=True)
df.reset_index(drop=True, inplace=True)

# Build the 3D X array: shape (samples, timesteps, features)
# Timestep 0 = 3 days ago, Timestep 1 = 2 days ago, Timestep 2 = yesterday
X_3d = np.zeros((len(df), 3, len(weather_cols)))
for i, col in enumerate(weather_cols):
    X_3d[:, 0, i] = df[f'{col}_lag_3']
    X_3d[:, 1, i] = df[f'{col}_lag_2']
    X_3d[:, 2, i] = df[f'{col}_lag_1']

y_precip = df['target_precipitation'].values
y_et0 = df['target_et0'].values

print(f'Input Shape: {X_3d.shape}')
"""),

    nbf.v4.new_markdown_cell("## 4. Train/Test Split & Normalization"),
    nbf.v4.new_code_cell("""split_idx = int(len(df) * 0.8)

X_train, X_test = X_3d[:split_idx], X_3d[split_idx:]
y_train_precip, y_test_precip = y_precip[:split_idx], y_precip[split_idx:]
y_train_et0, y_test_et0 = y_et0[:split_idx], y_et0[split_idx:]

# Normalize inputs using standard scaling
X_mean = X_train.mean(axis=0)
X_std = X_train.std(axis=0)
X_std[X_std == 0] = 1e-6 # prevent division by zero

X_train_scaled = (X_train - X_mean) / X_std
X_test_scaled = (X_test - X_mean) / X_std

np.save('X_mean_3d.npy', X_mean)
np.save('X_std_3d.npy', X_std)
print(f'Training on {len(X_train)} days, Testing on {len(X_test)} days.')
"""),

    nbf.v4.new_markdown_cell("## 5. Model Architectures"),
    nbf.v4.new_code_cell("""def create_dense_model(input_shape):
    model = Sequential([
        Input(shape=input_shape),
        Flatten(), # Flattens the 3x6 grid into 18 before processing
        Dense(16, activation='relu'),
        Dense(8, activation='relu'),
        Dense(1)
    ])
    model.compile(optimizer='adam', loss='mse')
    return model

def create_gru_model(input_shape):
    model = Sequential([
        Input(shape=input_shape),
        GRU(16, activation='tanh'),
        Dense(8, activation='relu'),
        Dense(1)
    ])
    model.compile(optimizer='adam', loss='mse')
    return model
"""),

    nbf.v4.new_markdown_cell("## 6. Train Models"),
    nbf.v4.new_code_cell("""input_shape = (3, len(weather_cols))

print("Training Dense Models...")
dense_precip = create_dense_model(input_shape)
dense_precip.fit(X_train_scaled, y_train_precip, epochs=50, batch_size=16, verbose=0)
dense_et0 = create_dense_model(input_shape)
dense_et0.fit(X_train_scaled, y_train_et0, epochs=50, batch_size=16, verbose=0)

print("Training GRU Models...")
gru_precip = create_gru_model(input_shape)
gru_precip.fit(X_train_scaled, y_train_precip, epochs=50, batch_size=16, verbose=0)
gru_et0 = create_gru_model(input_shape)
gru_et0.fit(X_train_scaled, y_train_et0, epochs=50, batch_size=16, verbose=0)

print("Training complete.")
"""),

    nbf.v4.new_markdown_cell("## 7. Performance Comparison"),
    nbf.v4.new_code_cell("""def evaluate_model(model, X_test, y_test, name):
    preds = model.predict(X_test, verbose=0).flatten()
    mae = mean_absolute_error(y_test, preds)
    r2 = r2_score(y_test, preds)
    return mae, r2

# Precipitation
dense_mae_p, dense_r2_p = evaluate_model(dense_precip, X_test_scaled, y_test_precip, "Dense Precip")
gru_mae_p, gru_r2_p = evaluate_model(gru_precip, X_test_scaled, y_test_precip, "GRU Precip")

# ET0
dense_mae_e, dense_r2_e = evaluate_model(dense_et0, X_test_scaled, y_test_et0, "Dense ET0")
gru_mae_e, gru_r2_e = evaluate_model(gru_et0, X_test_scaled, y_test_et0, "GRU ET0")

print("========== PRECIPITATION ==========")
print(f"Dense - MAE: {dense_mae_p:.3f} mm  | R²: {dense_r2_p:.3f}")
print(f"GRU   - MAE: {gru_mae_p:.3f} mm  | R²: {gru_r2_p:.3f}")
print("\\n========== EVAPOTRANSPIRATION ==========")
print(f"Dense - MAE: {dense_mae_e:.3f} mm  | R²: {dense_r2_e:.3f}")
print(f"GRU   - MAE: {gru_mae_e:.3f} mm  | R²: {gru_r2_e:.3f}")
""")
]

with open('weather_forecasting_model.ipynb', 'w') as f:
    nbf.write(nb, f)
