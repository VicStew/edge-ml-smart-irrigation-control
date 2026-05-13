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
    accuracy_score,
    average_precision_score,
    brier_score_loss,
    f1_score,
    mean_absolute_error,
    mean_squared_error,
    precision_score,
    r2_score,
    recall_score,
    roc_auc_score,
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
BASE_YEAR = 2020
LAG_HOURS = (1, 3, 6, 24)
LAG_GRAIN = "monthly"
SEED = 7


def cyclical_columns(
    values: pd.Series,
    period: float,
    prefix: str,
    harmonics: int = 1,
) -> dict[str, np.ndarray]:
    angles = 2 * np.pi * values.to_numpy(dtype=float) / period
    features = {}

    for harmonic in range(1, harmonics + 1):
        features[f"{prefix}_sin_{harmonic}"] = np.sin(harmonic * angles)
        features[f"{prefix}_cos_{harmonic}"] = np.cos(harmonic * angles)

    return features


def load_weather() -> pd.DataFrame:
    df = pd.read_csv(DATA_PATH)
    df["time"] = pd.to_datetime(df["time"])
    df = df.sort_values("time").reset_index(drop=True)
    df["rain_mm"] = df["rain_mm"].clip(lower=0)
    df["rain_occurrence"] = (df["rain_mm"] >= RAIN_THRESHOLD_MM).astype(np.float32)
    df["month_eat"] = df["time"].dt.month
    df["hour_eat"] = df["time"].dt.hour
    df["day_of_year_eat"] = df["time"].dt.dayofyear
    df["day_of_week_eat"] = df["time"].dt.dayofweek
    df["day_of_month_eat"] = df["time"].dt.day
    return df


def build_climatology(frame: pd.DataFrame) -> dict:
    month_hour = (
        frame.groupby(["month_eat", "hour_eat"])[["rain_mm", "rain_occurrence"]]
        .mean()
        .reset_index()
    )
    doy_hour = (
        frame.groupby(["day_of_year_eat", "hour_eat"])[["rain_mm", "rain_occurrence"]]
        .mean()
        .reset_index()
    )

    climatology = {
        "month_hour_amount": {
            (int(row.month_eat), int(row.hour_eat)): float(row.rain_mm)
            for row in month_hour.itertuples()
        },
        "month_hour_probability": {
            (int(row.month_eat), int(row.hour_eat)): float(row.rain_occurrence)
            for row in month_hour.itertuples()
        },
        "doy_hour_amount": {
            (int(row.day_of_year_eat), int(row.hour_eat)): float(row.rain_mm)
            for row in doy_hour.itertuples()
        },
        "doy_hour_probability": {
            (int(row.day_of_year_eat), int(row.hour_eat)): float(row.rain_occurrence)
            for row in doy_hour.itertuples()
        },
        "global_amount": float(frame["rain_mm"].mean()),
        "global_probability": float(frame["rain_occurrence"].mean()),
    }

    for lag_hour in LAG_HOURS:
        lagged = frame.copy()
        lagged["lag_time"] = lagged["time"] + pd.Timedelta(hours=lag_hour)
        lagged["lag_month"] = lagged["lag_time"].dt.month
        lagged["lag_hour"] = lagged["lag_time"].dt.hour
        lag_summary = (
            lagged.groupby(["lag_month", "lag_hour"])[["rain_mm", "rain_occurrence"]]
            .mean()
            .reset_index()
        )
        climatology[f"lag_{lag_hour}h_amount_{LAG_GRAIN}"] = {
            (int(row.lag_month), int(row.lag_hour)): float(row.rain_mm)
            for row in lag_summary.itertuples()
        }
        climatology[f"lag_{lag_hour}h_probability_{LAG_GRAIN}"] = {
            (int(row.lag_month), int(row.lag_hour)): float(row.rain_occurrence)
            for row in lag_summary.itertuples()
        }

    return climatology


def build_features(timestamps: pd.Series, climatology: dict) -> pd.DataFrame:
    ts = pd.to_datetime(timestamps)
    month = ts.dt.month
    hour = ts.dt.hour
    minute = ts.dt.minute
    day_of_year = ts.dt.dayofyear
    day_of_week = ts.dt.dayofweek
    day_of_month = ts.dt.day
    year_index = ts.dt.year - BASE_YEAR

    features = {
        "year_index": year_index.to_numpy(dtype=float),
        "is_weekend": (day_of_week >= 5).astype(float).to_numpy(),
        "month_hour_amount_climatology": np.array(
            [
                climatology["month_hour_amount"].get(
                    key,
                    climatology["global_amount"],
                )
                for key in zip(month, hour)
            ]
        ),
        "month_hour_probability_climatology": np.array(
            [
                climatology["month_hour_probability"].get(
                    key,
                    climatology["global_probability"],
                )
                for key in zip(month, hour)
            ]
        ),
        "doy_hour_amount_climatology": np.array(
            [
                climatology["doy_hour_amount"].get(
                    key,
                    climatology["global_amount"],
                )
                for key in zip(day_of_year, hour)
            ]
        ),
        "doy_hour_probability_climatology": np.array(
            [
                climatology["doy_hour_probability"].get(
                    key,
                    climatology["global_probability"],
                )
                for key in zip(day_of_year, hour)
            ]
        ),
    }

    for lag_hour in LAG_HOURS:
        lagged_time = ts - pd.Timedelta(hours=lag_hour)
        lag_keys = list(zip(lagged_time.dt.month, lagged_time.dt.hour))
        amount_key = f"lag_{lag_hour}h_amount_{LAG_GRAIN}"
        probability_key = f"lag_{lag_hour}h_probability_{LAG_GRAIN}"
        features[amount_key] = np.array(
            [
                climatology[amount_key].get(key, climatology["global_amount"])
                for key in lag_keys
            ]
        )
        features[probability_key] = np.array(
            [
                climatology[probability_key].get(
                    key,
                    climatology["global_probability"],
                )
                for key in lag_keys
            ]
        )

    features.update(cyclical_columns(month, 12.0, "month"))
    features.update(cyclical_columns(hour + minute / 60.0, 24.0, "hour", harmonics=2))
    features.update(cyclical_columns(day_of_year, 365.25, "doy", harmonics=2))
    features.update(cyclical_columns(day_of_week, 7.0, "dow"))
    features.update(cyclical_columns(day_of_month, 31.0, "dom"))

    return pd.DataFrame(features, index=ts.index).astype(np.float32)


def build_model(feature_count: int) -> tf.keras.Model:
    inputs = tf.keras.Input(shape=(feature_count,))
    x = tf.keras.layers.Dense(32, activation="relu")(inputs)
    x = tf.keras.layers.Dense(16, activation="relu")(x)
    amount = tf.keras.layers.Dense(1, name="amount")(x)
    probability = tf.keras.layers.Dense(1, activation="sigmoid", name="probability")(x)
    model = tf.keras.Model(inputs, [amount, probability])
    model.compile(
        optimizer=tf.keras.optimizers.Adam(learning_rate=0.001),
        loss={
            "amount": "mse",
            "probability": "binary_crossentropy",
        },
        loss_weights={
            "amount": 1.0,
            "probability": 0.5,
        },
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
        "rainy_mae_mm": float(mean_absolute_error(rainy_true, rainy_pred)),
        "rainy_rmse_mm": float(mean_squared_error(rainy_true, rainy_pred) ** 0.5),
    }


def probability_metrics(y_true: np.ndarray, y_prob: np.ndarray) -> dict:
    y_pred = (y_prob >= 0.5).astype(np.float32)
    positives = int(y_true.sum())
    negatives = int(len(y_true) - positives)
    return {
        "brier_score": float(brier_score_loss(y_true, y_prob)),
        "accuracy": float(accuracy_score(y_true, y_pred)),
        "precision": float(precision_score(y_true, y_pred, zero_division=0)),
        "recall": float(recall_score(y_true, y_pred, zero_division=0)),
        "f1": float(f1_score(y_true, y_pred, zero_division=0)),
        "roc_auc": float(roc_auc_score(y_true, y_prob)),
        "pr_auc": float(average_precision_score(y_true, y_prob)),
        "positive_hours": positives,
        "negative_hours": negatives,
    }


def format_float(value: float) -> str:
    return f"{np.float32(value):.9g}f"


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


def table_values(
    table: dict,
    first_count: int,
    fallback: float,
) -> list[float]:
    return [
        table.get((first_index, hour), fallback)
        for first_index in range(1, first_count + 1)
        for hour in range(24)
    ]


def write_preprocessing_header(
    climatology: dict,
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
        "static const uint16_t HOURLY_RAINFALL_MONTH_HOUR_COUNT = 288;",
        "static const uint16_t HOURLY_RAINFALL_DOY_HOUR_COUNT = 8784;",
        f"static const int HOURLY_RAINFALL_BASE_YEAR = {BASE_YEAR};",
        f"static const float HOURLY_RAINFALL_Y_AMOUNT_MEAN = {format_float(y_amount_mean)};",
        f"static const float HOURLY_RAINFALL_Y_AMOUNT_STD = {format_float(y_amount_std)};",
        f"static const float HOURLY_RAINFALL_GLOBAL_AMOUNT = {format_float(climatology['global_amount'])};",
        f"static const float HOURLY_RAINFALL_GLOBAL_PROBABILITY = {format_float(climatology['global_probability'])};",
        "",
    ]

    write_float_array(lines, "HOURLY_RAINFALL_X_MEAN", x_mean, columns=10)
    write_float_array(lines, "HOURLY_RAINFALL_X_STD", x_std, columns=10)
    write_float_array(
        lines,
        "HOURLY_RAINFALL_MONTH_HOUR_AMOUNT",
        table_values(
            climatology["month_hour_amount"],
            12,
            climatology["global_amount"],
        ),
    )
    write_float_array(
        lines,
        "HOURLY_RAINFALL_MONTH_HOUR_PROBABILITY",
        table_values(
            climatology["month_hour_probability"],
            12,
            climatology["global_probability"],
        ),
    )
    write_float_array(
        lines,
        "HOURLY_RAINFALL_DOY_HOUR_AMOUNT",
        table_values(
            climatology["doy_hour_amount"],
            366,
            climatology["global_amount"],
        ),
    )
    write_float_array(
        lines,
        "HOURLY_RAINFALL_DOY_HOUR_PROBABILITY",
        table_values(
            climatology["doy_hour_probability"],
            366,
            climatology["global_probability"],
        ),
    )

    for lag_hour in LAG_HOURS:
        write_float_array(
            lines,
            f"HOURLY_RAINFALL_LAG_{lag_hour}H_MONTH_AMOUNT",
            table_values(
                climatology[f"lag_{lag_hour}h_amount_{LAG_GRAIN}"],
                12,
                climatology["global_amount"],
            ),
        )
        write_float_array(
            lines,
            f"HOURLY_RAINFALL_LAG_{lag_hour}H_MONTH_PROBABILITY",
            table_values(
                climatology[f"lag_{lag_hour}h_probability_{LAG_GRAIN}"],
                12,
                climatology["global_probability"],
            ),
        )

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
    climatology = build_climatology(train_df)

    x_train_df = build_features(train_df["time"], climatology)
    x_test_df = build_features(test_df["time"], climatology)
    feature_columns = list(x_train_df.columns)
    x_train = x_train_df.to_numpy(dtype=np.float32)
    x_test = x_test_df.to_numpy(dtype=np.float32)
    x_mean = x_train.mean(axis=0)
    x_std = x_train.std(axis=0)
    x_std[x_std == 0] = 1e-6
    x_train_scaled = (x_train - x_mean) / x_std
    x_test_scaled = (x_test - x_mean) / x_std

    y_train_amount = train_df["rain_mm"].to_numpy(dtype=np.float32)
    y_test_amount = test_df["rain_mm"].to_numpy(dtype=np.float32)
    y_train_rain = train_df["rain_occurrence"].to_numpy(dtype=np.float32)
    y_test_rain = test_df["rain_occurrence"].to_numpy(dtype=np.float32)
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
        {
            "amount": y_train_amount_scaled,
            "probability": y_train_rain,
        },
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

    amount_scaled, probability = model.predict(x_test_scaled, verbose=0)
    amount_pred = np.expm1(
        ((amount_scaled.reshape(-1) * y_amount_std) + y_amount_mean).clip(min=0)
    )
    probability_pred = probability.reshape(-1).clip(0, 1)

    metrics = {
        "train_rows": int(len(train_df)),
        "test_rows": int(len(test_df)),
        "lag_feature_strategy": LAG_GRAIN,
        "lag_hours": list(LAG_HOURS),
        "feature_columns": feature_columns,
        "amount_models": {
            "mlp_monthly_lags": amount_metrics(y_test_amount, amount_pred),
        },
        "probability_models": {
            "mlp_monthly_lags": probability_metrics(y_test_rain, probability_pred),
        },
        "selected_amount_model": "mlp_monthly_lags",
        "selected_probability_model": "mlp_monthly_lags",
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
        climatology,
        x_mean,
        x_std,
        y_amount_mean,
        y_amount_std,
    )

    np.save(X_MEAN_PATH, x_mean)
    np.save(X_STD_PATH, x_std)
    np.save(Y_AMOUNT_MEAN_PATH, np.array([y_amount_mean], dtype=np.float32))
    np.save(Y_AMOUNT_STD_PATH, np.array([y_amount_std], dtype=np.float32))
    FEATURE_COLUMNS_PATH.write_text(json.dumps(feature_columns, indent=2) + "\n")
    METRICS_PATH.write_text(json.dumps(metrics, indent=2) + "\n")

    print(json.dumps(metrics, indent=2))


if __name__ == "__main__":
    main()
