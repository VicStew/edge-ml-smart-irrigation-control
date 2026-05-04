from __future__ import annotations

from pathlib import Path
import sys

import pandas as pd
import requests


BASE_DIR = Path(__file__).resolve().parent
DAILY_OUTPUT_CSV = BASE_DIR / "historical_weather_data.csv"
HOURLY_OUTPUT_CSV = BASE_DIR / "historical_weather_data_hourly.csv"
URL = "https://archive-api.open-meteo.com/v1/archive"
PARAMS = {
    "latitude": -0.4167,
    "longitude": 36.9500,
    "start_date": "2022-01-01",
    "end_date": "2025-12-31",
    "daily": (
        "temperature_2m_max,temperature_2m_min,precipitation_sum,"
        "et0_fao_evapotranspiration,shortwave_radiation_sum"
    ),
    "hourly": (
        "precipitation_probability,rain,cloud_cover,et0_fao_evapotranspiration,"
        "soil_temperature_0_to_7cm,soil_moisture_0_to_7cm"
    ),
    "timezone": "Africa/Nairobi",
}


def fetch_weather_payload() -> dict:
    try:
        response = requests.get(URL, params=PARAMS, timeout=60)
        response.raise_for_status()
    except requests.exceptions.RequestException as exc:
        raise RuntimeError(
            "Failed to download weather history from Open-Meteo. "
            "This environment currently cannot reach archive-api.open-meteo.com."
        ) from exc
    return response.json()


def build_hourly_dataframe(payload: dict) -> pd.DataFrame:
    if "hourly" not in payload:
        raise ValueError("Hourly data not found in Open-Meteo response.")

    df_hourly = pd.DataFrame(payload["hourly"])
    required_columns = {
        "time",
        "precipitation_probability",
        "rain",
        "cloud_cover",
        "et0_fao_evapotranspiration",
        "soil_temperature_0_to_7cm",
        "soil_moisture_0_to_7cm",
    }
    missing_columns = sorted(required_columns.difference(df_hourly.columns))
    if missing_columns:
        raise ValueError(f"Hourly response is missing expected columns: {missing_columns}")

    df_hourly["time"] = pd.to_datetime(df_hourly["time"])
    df_hourly["date"] = df_hourly["time"].dt.strftime("%Y-%m-%d")
    df_hourly["hour"] = df_hourly["time"].dt.hour
    df_hourly["month"] = df_hourly["time"].dt.month
    df_hourly["day_of_year"] = df_hourly["time"].dt.dayofyear
    df_hourly["probability_of_rain_percent"] = df_hourly["precipitation_probability"].clip(lower=0, upper=100)
    df_hourly["rain_mm"] = df_hourly["rain"].clip(lower=0)
    df_hourly["cloud_cover_total"] = df_hourly["cloud_cover"].clip(lower=0, upper=100)
    df_hourly["rain_occurrence"] = (df_hourly["rain_mm"] >= 0.1).astype(int)

    ordered_columns = [
        "time",
        "date",
        "hour",
        "month",
        "day_of_year",
        "precipitation_probability",
        "probability_of_rain_percent",
        "rain",
        "rain_mm",
        "cloud_cover",
        "cloud_cover_total",
        "et0_fao_evapotranspiration",
        "soil_temperature_0_to_7cm",
        "soil_moisture_0_to_7cm",
        "rain_occurrence",
    ]
    return df_hourly[ordered_columns]


def build_daily_dataframe(payload: dict, hourly_df: pd.DataFrame) -> pd.DataFrame:
    if "daily" not in payload:
        raise ValueError("Daily data not found in Open-Meteo response.")

    df_daily = pd.DataFrame(payload["daily"])
    daily_aggregated = hourly_df.groupby("date").mean(numeric_only=True).reset_index()
    daily_aggregated.rename(columns={"date": "time"}, inplace=True)
    df_merged = pd.merge(df_daily, daily_aggregated, on="time", how="inner")
    df_merged["precipitation"] = df_merged["precipitation_sum"]
    df_merged["evapotranspiration"] = df_merged["et0_fao_evapotranspiration"]

    ordered_columns = [
        "time",
        "temperature_2m_max",
        "temperature_2m_min",
        "shortwave_radiation_sum",
        "precipitation_probability",
        "rain",
        "cloud_cover",
        "precipitation_sum",
        "et0_fao_evapotranspiration",
        "soil_temperature_0_to_7cm",
        "soil_moisture_0_to_7cm",
        "precipitation",
        "evapotranspiration",
    ]
    return df_merged[ordered_columns]


def main() -> None:
    print("Fetching hourly and daily weather history for Nyeri County...")
    try:
        payload = fetch_weather_payload()
        hourly_df = build_hourly_dataframe(payload)
        daily_df = build_daily_dataframe(payload, hourly_df)
    except Exception as exc:
        print(str(exc))
        sys.exit(1)

    hourly_df.to_csv(HOURLY_OUTPUT_CSV, index=False)
    daily_df.to_csv(DAILY_OUTPUT_CSV, index=False)

    print(f"Saved hourly history to {HOURLY_OUTPUT_CSV.name} ({len(hourly_df)} rows)")
    print(f"Saved daily aggregate to {DAILY_OUTPUT_CSV.name} ({len(daily_df)} rows)")


if __name__ == "__main__":
    main()
