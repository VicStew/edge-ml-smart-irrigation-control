#!/usr/bin/env python3
"""Train and export the hourly rainfall model used by the ESP32 firmware."""

from __future__ import annotations

import json
import os
import random
from pathlib import Path

import numpy as np
import pandas as pd
import tensorflow as tf
from sklearn.metrics import (
    mean_absolute_error,
    mean_squared_error,
    r2_score,
)

os.environ.setdefault("TF_CPP_MIN_LOG_LEVEL", "2")

BASE_DIR = Path(__file__).resolve().parent
PROJECT_DIR = BASE_DIR.parent
MASTER_ESP_DIR = PROJECT_DIR / "master_esp"

DATA_PATH = BASE_DIR / "historical_weather_data_hourly.csv"
KERAS_MODEL_PATH = BASE_DIR / "hourly_rainfall_forecaster.keras"
TFLITE_MODEL_PATH = BASE_DIR / "hourly_rainfall_forecaster.tflite"
CC_MODEL_PATH = BASE_DIR / "hourly_rainfall_forecaster.cc"
H_MODEL_PATH = BASE_DIR / "hourly_rainfall_forecaster.h"
FEATURE_COLUMNS_PATH = BASE_DIR / "hourly_feature_columns.json"
METRICS_PATH = BASE_DIR / "hourly_rainfall_metrics.json"
X_MEAN_PATH = BASE_DIR / "hourly_X_mean.npy"
X_STD_PATH = BASE_DIR / "hourly_X_std.npy"
Y_AMOUNT_MEAN_PATH = BASE_DIR / "hourly_y_amount_mean.npy"
Y_AMOUNT_STD_PATH = BASE_DIR / "hourly_y_amount_std.npy"
ESP_PREPROCESSING_PATH = MASTER_ESP_DIR / "hourly_rainfall_preprocessing.h"
ESP_CC_MODEL_PATH = MASTER_ESP_DIR / "hourly_rainfall_forecaster.cpp"
ESP_H_MODEL_PATH = MASTER_ESP_DIR / "hourly_rainfall_forecaster.h"

C_ARRAY_NAME = "hourly_rainfall_forecaster_tflite"
RAIN_THRESHOLD_MM = 0.1
SEED = 7

FEATURES = [
    "temperature_2m",
    "relative_humidity_2m",
    "soil_temperature_0_to_7cm",
    "soil_moisture_0_to_7cm",
    "shortwave_radiation",
    "et0_fao_evapotranspiration_mm",
]


def load_weather() -> pd.DataFrame:
    df = pd.read_csv(DATA_PATH)
    df["time"] = pd.to_datetime(df["time"])
    df = df.sort_values("time").reset_index(drop=True)
    df["precipitation_mm"] = df["precipitation_mm"].clip(lower=0)
    df = df.dropna(subset=FEATURES + ["precipitation_mm"])
    return df


def build_model(feature_count: int) -> tf.keras.Model:
    inputs = tf.keras.Input(shape=(feature_count,))
    x = tf.keras.layers.Dense(32, activation="relu")(inputs)
    x = tf.keras.layers.Dense(16, activation="relu")(x)
    amount = tf.keras.layers.Dense(1, name="amount")(x)
    model = tf.keras.Model(inputs, amount)
    model.compile(
        optimizer=tf.keras.optimizers.Adam(learning_rate=0.001),
        loss="mse",
    )
    return model


def amount_metrics(y_true: np.ndarray, y_pred: np.ndarray) -> dict:
    rainy_mask = y_true >= RAIN_THRESHOLD_MM
    rainy_true = y_true[rainy_mask]
    rainy_pred = y_pred[rainy_mask]
    return {
        "mae_mm": float(mean_absolute_error(y_true, y_pred)),
        "rmse_mm": float(mean_squared_error(y_true, y_pred) ** 0.5),
        "r2": float(r2_score(y_true, y_pred)),
        "rainy_hours_count": int(rainy_mask.sum()),
        "rainy_mae_mm": float(mean_absolute_error(rainy_true, rainy_pred)) if len(rainy_true) > 0 else 0.0,
        "rainy_rmse_mm": float(mean_squared_error(rainy_true, rainy_pred) ** 0.5) if len(rainy_true) > 0 else 0.0,
    }


def format_float(value: float) -> str:
    literal = f"{np.float32(value):.9g}"
    if "." not in literal and "e" not in literal.lower():
        literal += ".0"
    return f"{literal}f"


def write_float_array(
    lines: list[str],
    name: str,
    values: list[float] | np.ndarray,
    columns: int = 8,
) -> None:
    lines.append(f"static const float {name}[{len(values)}] = {{")
    for start in range(0, len(values), columns):
        chunk = values[start : start + columns]
        lines.append("  " + ", ".join(format_float(v) for v in chunk) + ",")
    lines.append("};")
    lines.append("")


def write_preprocessing_header(
    x_mean: np.ndarray,
    x_std: np.ndarray,
    y_amount_mean: float,
    y_amount_std: float,
) -> None:
    lines = [
        "#ifndef HOURLY_RAINFALL_PREPROCESSING_H",
        "#define HOURLY_RAINFALL_PREPROCESSING_H",
        "",
        "#include <stdint.h>",
        "",
        f"static const uint8_t HOURLY_RAINFALL_FEATURE_COUNT = {len(x_mean)};",
        f"static const float HOURLY_RAINFALL_Y_AMOUNT_MEAN = {format_float(y_amount_mean)};",
        f"static const float HOURLY_RAINFALL_Y_AMOUNT_STD = {format_float(y_amount_std)};",
        "",
    ]

    write_float_array(lines, "HOURLY_RAINFALL_X_MEAN", x_mean, columns=10)
    write_float_array(lines, "HOURLY_RAINFALL_X_STD", x_std, columns=10)

    lines.append("#endif  // HOURLY_RAINFALL_PREPROCESSING_H")
    ESP_PREPROCESSING_PATH.write_text("\n".join(lines) + "\n")


def convert_tflite_to_c_array(
    tflite_path: Path,
    c_file_path: Path,
    h_file_path: Path,
    array_name: str,
) -> None:
    tflite_content = tflite_path.read_bytes()
    hex_array = [f"0x{byte:02x}" for byte in tflite_content]
    include_name = h_file_path.name

    with c_file_path.open("w") as c_file:
        c_file.write(f'#include "{include_name}"\n\n')
        c_file.write(f"const unsigned char {array_name}[] = {{\n")
        for idx in range(0, len(hex_array), 12):
            c_file.write("  " + ", ".join(hex_array[idx : idx + 12]) + ",\n")
        c_file.write("};\n\n")
        c_file.write(f"const int {array_name}_len = {len(tflite_content)};\n")

    with h_file_path.open("w") as h_file:
        guard = h_file_path.stem.upper() + "_H"
        h_file.write(f"#ifndef {guard}\n")
        h_file.write(f"#define {guard}\n\n")
        h_file.write(f"extern const unsigned char {array_name}[];\n")
        h_file.write(f"extern const int {array_name}_len;\n\n")
        h_file.write(f"#endif  // {guard}\n")


def main() -> None:
    random.seed(SEED)
    np.random.seed(SEED)
    tf.random.set_seed(SEED)

    df = load_weather()
    split_idx = int(len(df) * 0.8)
    train_df = df.iloc[:split_idx].copy()
    test_df = df.iloc[split_idx:].copy()

    x_train = train_df[FEATURES].to_numpy(dtype=np.float32)
    x_test = test_df[FEATURES].to_numpy(dtype=np.float32)
    x_mean = x_train.mean(axis=0)
    x_std = x_train.std(axis=0)
    x_std[x_std == 0] = 1e-6
    x_train_scaled = (x_train - x_mean) / x_std
    x_test_scaled = (x_test - x_mean) / x_std

    y_train_amount = train_df["precipitation_mm"].to_numpy(dtype=np.float32)
    y_test_amount = test_df["precipitation_mm"].to_numpy(dtype=np.float32)
    
    y_train_amount_log = np.log1p(y_train_amount)
    y_amount_mean = float(y_train_amount_log.mean())
    y_amount_std = float(y_train_amount_log.std())
    if y_amount_std == 0:
        y_amount_std = 1e-6
    y_train_amount_scaled = (
        (y_train_amount_log - y_amount_mean) / y_amount_std
    ).astype(np.float32)

    model = build_model(x_train_scaled.shape[1])
    model.fit(
        x_train_scaled,
        y_train_amount_scaled,
        validation_split=0.15,
        epochs=40,
        batch_size=256,
        verbose=1,
        callbacks=[
            tf.keras.callbacks.EarlyStopping(
                monitor="val_loss",
                patience=5,
                restore_best_weights=True,
            )
        ],
    )

    amount_scaled = model.predict(x_test_scaled, verbose=0)
    amount_pred = np.expm1(
        ((amount_scaled.reshape(-1) * y_amount_std) + y_amount_mean).clip(min=0)
    )

    metrics = {
        "train_rows": int(len(train_df)),
        "test_rows": int(len(test_df)),
        "feature_columns": FEATURES,
        "amount_models": {
            "mlp_sensors": amount_metrics(y_test_amount, amount_pred),
        },
        "selected_amount_model": "mlp_sensors",
    }

    model.save(KERAS_MODEL_PATH)
    converter = tf.lite.TFLiteConverter.from_keras_model(model)
    tflite_model = converter.convert()
    TFLITE_MODEL_PATH.write_bytes(tflite_model)
    convert_tflite_to_c_array(TFLITE_MODEL_PATH, CC_MODEL_PATH, H_MODEL_PATH, C_ARRAY_NAME)
    convert_tflite_to_c_array(
        TFLITE_MODEL_PATH,
        ESP_CC_MODEL_PATH,
        ESP_H_MODEL_PATH,
        C_ARRAY_NAME,
    )
    write_preprocessing_header(
        x_mean,
        x_std,
        y_amount_mean,
        y_amount_std,
    )

    np.save(X_MEAN_PATH, x_mean)
    np.save(X_STD_PATH, x_std)
    np.save(Y_AMOUNT_MEAN_PATH, np.array([y_amount_mean], dtype=np.float32))
    np.save(Y_AMOUNT_STD_PATH, np.array([y_amount_std], dtype=np.float32))
    FEATURE_COLUMNS_PATH.write_text(json.dumps(FEATURES, indent=2) + "\n")
    METRICS_PATH.write_text(json.dumps(metrics, indent=2) + "\n")

    print(json.dumps(metrics, indent=2))


if __name__ == "__main__":
    main()
