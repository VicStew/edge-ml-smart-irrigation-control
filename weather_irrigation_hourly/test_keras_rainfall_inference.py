#!/usr/bin/env python3
"""Test saved Keras rainfall model inference with the training preprocessing."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
import pandas as pd
import tensorflow as tf


BASE_DIR = Path(__file__).resolve().parent
DATA_PATH = BASE_DIR / "historical_weather_data_hourly.csv"
KERAS_MODEL_PATH = BASE_DIR / "hourly_rainfall_forecaster.keras"
FEATURE_COLUMNS_PATH = BASE_DIR / "hourly_feature_columns.json"
X_MEAN_PATH = BASE_DIR / "hourly_X_mean.npy"
X_STD_PATH = BASE_DIR / "hourly_X_std.npy"
Y_AMOUNT_MEAN_PATH = BASE_DIR / "hourly_y_amount_mean.npy"
Y_AMOUNT_STD_PATH = BASE_DIR / "hourly_y_amount_std.npy"
LAG_HOURS = (1, 3, 6, 24)
LAG_GRAIN = "monthly"
BASE_YEAR = 2020


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


def build_climatology(frame: pd.DataFrame) -> dict:
    train = frame.copy()
    month_hour = (
        train.groupby(["month_eat", "hour_eat"])[["rain_mm", "rain_occurrence"]]
        .mean()
        .reset_index()
    )
    doy_hour = (
        train.groupby(["day_of_year_eat", "hour_eat"])[["rain_mm", "rain_occurrence"]]
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
        "global_amount": float(train["rain_mm"].mean()),
        "global_probability": float(train["rain_occurrence"].mean()),
    }

    for lag_hour in LAG_HOURS:
        lagged = train.copy()
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
    month_hour_keys = list(zip(month, hour))
    doy_hour_keys = list(zip(day_of_year, hour))

    features = {
        "year_index": year_index.to_numpy(dtype=float),
        "is_weekend": (day_of_week >= 5).astype(float).to_numpy(),
        "month_hour_amount_climatology": np.array(
            [
                climatology["month_hour_amount"].get(
                    key,
                    climatology["global_amount"],
                )
                for key in month_hour_keys
            ]
        ),
        "month_hour_probability_climatology": np.array(
            [
                climatology["month_hour_probability"].get(
                    key,
                    climatology["global_probability"],
                )
                for key in month_hour_keys
            ]
        ),
        "doy_hour_amount_climatology": np.array(
            [
                climatology["doy_hour_amount"].get(
                    key,
                    climatology["global_amount"],
                )
                for key in doy_hour_keys
            ]
        ),
        "doy_hour_probability_climatology": np.array(
            [
                climatology["doy_hour_probability"].get(
                    key,
                    climatology["global_probability"],
                )
                for key in doy_hour_keys
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


def load_context() -> tuple[pd.DataFrame, pd.DataFrame, dict, list[str]]:
    df = pd.read_csv(DATA_PATH)
    df["time"] = pd.to_datetime(df["time"])
    df = df.sort_values("time").reset_index(drop=True)

    split_idx = int(len(df) * 0.8)
    train_df = df.iloc[:split_idx].copy()
    test_df = df.iloc[split_idx:].copy()
    climatology = build_climatology(train_df)
    feature_columns = json.loads(FEATURE_COLUMNS_PATH.read_text())

    return train_df, test_df, climatology, feature_columns


def predict_rainfall(
    model: tf.keras.Model,
    timestamps: list[str],
    climatology: dict,
    feature_columns: list[str],
) -> pd.DataFrame:
    ts = pd.Series(pd.to_datetime(timestamps), name="forecast_time")
    x_mean = np.load(X_MEAN_PATH)
    x_std = np.load(X_STD_PATH)
    y_amount_mean = float(np.load(Y_AMOUNT_MEAN_PATH)[0])
    y_amount_std = float(np.load(Y_AMOUNT_STD_PATH)[0])

    features = build_features(ts, climatology)[feature_columns]
    x = features.to_numpy(dtype=np.float32)
    x_scaled = (x - x_mean) / x_std

    amount_scaled, probability = model.predict(x_scaled, verbose=0)
    amount_scaled = amount_scaled.reshape(-1)
    probability = probability.reshape(-1)
    rainfall_mm = np.expm1((amount_scaled * y_amount_std) + y_amount_mean).clip(min=0)

    return pd.DataFrame(
        {
            "forecast_time": ts,
            "rainfall_mm": rainfall_mm,
            "rain_probability": probability.clip(0, 1),
        }
    )


def evaluate_test_set(
    model: tf.keras.Model,
    test_df: pd.DataFrame,
    climatology: dict,
    feature_columns: list[str],
) -> None:
    predictions = predict_rainfall(
        model,
        test_df["time"].astype(str).tolist(),
        climatology,
        feature_columns,
    )
    actual_amount = test_df["rain_mm"].to_numpy(dtype=np.float32)
    actual_rain = test_df["rain_occurrence"].to_numpy(dtype=np.float32)
    pred_amount = predictions["rainfall_mm"].to_numpy(dtype=np.float32)
    pred_probability = predictions["rain_probability"].to_numpy(dtype=np.float32)
    pred_rain = (pred_probability >= 0.5).astype(np.float32)

    mae = float(np.mean(np.abs(actual_amount - pred_amount)))
    rmse = float(np.sqrt(np.mean((actual_amount - pred_amount) ** 2)))
    accuracy = float(np.mean(actual_rain == pred_rain))

    print("\nTest-set check")
    print(f"Rows: {len(test_df):,}")
    print(f"Rainfall MAE: {mae:.4f} mm")
    print(f"Rainfall RMSE: {rmse:.4f} mm")
    print(f"Rain occurrence accuracy @ 0.50: {accuracy:.4f}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run inference with the saved Keras hourly rainfall model."
    )
    parser.add_argument(
        "timestamps",
        nargs="*",
        help="Forecast timestamps, for example '2026-05-12 15:00:00'.",
    )
    parser.add_argument(
        "--evaluate",
        action="store_true",
        help="Also evaluate the saved Keras model on the chronological test split.",
    )
    parser.add_argument(
        "--next-hour",
        action="store_true",
        help="Shift supplied timestamps one hour ahead before inference.",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    _, test_df, climatology, feature_columns = load_context()
    model = tf.keras.models.load_model(KERAS_MODEL_PATH)

    timestamps = args.timestamps
    if not timestamps:
        timestamps = test_df["time"].tail(3).astype(str).tolist()

    ts = pd.to_datetime(pd.Series(timestamps))
    if args.next_hour:
        ts = ts + pd.Timedelta(hours=1)

    predictions = predict_rainfall(
        model,
        ts.astype(str).tolist(),
        climatology,
        feature_columns,
    )

    print("Keras rainfall predictions")
    print(predictions.to_string(index=False))

    if args.evaluate:
        evaluate_test_set(model, test_df, climatology, feature_columns)


if __name__ == "__main__":
    main()
