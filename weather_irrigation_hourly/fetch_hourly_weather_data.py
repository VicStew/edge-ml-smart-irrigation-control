from __future__ import annotations

import argparse
from io import StringIO
from pathlib import Path
import sys
from zoneinfo import ZoneInfo

import pandas as pd
import requests


BASE_DIR = Path(__file__).resolve().parent
OUTPUT_CSV = BASE_DIR / "historical_weather_data_hourly.csv"
EAT = ZoneInfo("Africa/Nairobi")
UTC = ZoneInfo("UTC")
URL = "https://archive-api.open-meteo.com/v1/archive"
DEFAULT_EXPORT_PATH = Path("/home/vector/Downloads/open-meteo-0.39S36.94E1815m.csv")
PARAMS = {
    "latitude": -0.4167,
    "longitude": 36.9500,
    "start_date": "2022-01-01",
    "end_date": "2025-12-31",
    "hourly": (
        "precipitation_probability,rain,cloud_cover,"
        "evapotranspiration,soil_temperature_6cm,soil_moisture_0_to_1cm"
    ),
    "timezone": "GMT",
}

EXPORT_RENAME_MAP = {
    "precipitation_probability (%)": "precipitation_probability",
    "rain (mm)": "rain_mm",
    "cloud_cover (%)": "cloud_cover",
    "evapotranspiration (mm)": "evapotranspiration_mm",
    "soil_temperature_6cm (°C)": "soil_temperature_c",
    "soil_moisture_0_to_1cm (m³/m³)": "soil_moisture_m3m3",
}


def parse_export_csv(export_path: Path) -> pd.DataFrame:
    raw_text = export_path.read_text()
    parts = raw_text.split("\n\n", maxsplit=1)
    if len(parts) != 2:
        raise ValueError("Unexpected Open-Meteo export format: missing metadata separator.")

    metadata_df = pd.read_csv(StringIO(parts[0]))
    data_df = pd.read_csv(StringIO(parts[1]))
    timezone_name = str(metadata_df.loc[0, "timezone"]).strip()
    source_tz = ZoneInfo("UTC") if timezone_name in {"GMT", "UTC"} else ZoneInfo(timezone_name)
    return normalize_hourly_dataframe(data_df, source_tz=source_tz)


def fetch_api_dataframe() -> pd.DataFrame:
    try:
        response = requests.get(URL, params=PARAMS, timeout=60)
        response.raise_for_status()
    except requests.exceptions.RequestException as exc:
        raise RuntimeError(
            "Failed to download hourly weather history from Open-Meteo. "
            "Use --source-export with a local Open-Meteo CSV export when network access is unavailable."
        ) from exc

    payload = response.json()
    if "hourly" not in payload:
        raise ValueError("Hourly data not found in Open-Meteo API response.")
    data_df = pd.DataFrame(payload["hourly"])
    return normalize_hourly_dataframe(data_df, source_tz=UTC)


def normalize_hourly_dataframe(data_df: pd.DataFrame, source_tz: ZoneInfo) -> pd.DataFrame:
    df = data_df.rename(columns=EXPORT_RENAME_MAP).copy()
    required_columns = {
        "time",
        "precipitation_probability",
        "rain_mm",
        "cloud_cover",
        "evapotranspiration_mm",
        "soil_temperature_c",
        "soil_moisture_m3m3",
    }
    missing_columns = sorted(required_columns.difference(df.columns))
    if missing_columns:
        raise ValueError(f"Hourly weather data is missing expected columns: {missing_columns}")

    timestamps = pd.to_datetime(df["time"])
    if timestamps.dt.tz is None:
        timestamps = timestamps.dt.tz_localize(source_tz)
    else:
        timestamps = timestamps.dt.tz_convert(source_tz)
    timestamps_eat = timestamps.dt.tz_convert(EAT)

    df["time_utc"] = timestamps.dt.strftime("%Y-%m-%dT%H:%M:%SZ")
    df["time"] = timestamps_eat.dt.strftime("%Y-%m-%dT%H:%M:%S")
    df["date_eat"] = timestamps_eat.dt.strftime("%Y-%m-%d")
    df["hour_eat"] = timestamps_eat.dt.hour
    df["month_eat"] = timestamps_eat.dt.month
    df["day_of_year_eat"] = timestamps_eat.dt.dayofyear
    df["probability_of_rain_percent"] = df["precipitation_probability"].clip(lower=0, upper=100)
    df["rain_mm"] = df["rain_mm"].clip(lower=0)
    df["cloud_cover_total_percent"] = df["cloud_cover"].clip(lower=0, upper=100)
    df["rain_occurrence"] = (df["rain_mm"] >= 0.1).astype(int)

    ordered_columns = [
        "time",
        "time_utc",
        "date_eat",
        "hour_eat",
        "month_eat",
        "day_of_year_eat",
        "precipitation_probability",
        "probability_of_rain_percent",
        "rain_mm",
        "cloud_cover",
        "cloud_cover_total_percent",
        "evapotranspiration_mm",
        "soil_temperature_c",
        "soil_moisture_m3m3",
        "rain_occurrence",
    ]
    return df[ordered_columns]


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Fetch or normalize hourly weather history for rainfall modeling.")
    parser.add_argument(
        "--source-export",
        type=Path,
        default=None,
        help="Path to a local Open-Meteo CSV export. If omitted, the script tries the API first.",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=OUTPUT_CSV,
        help="Destination CSV for normalized EAT hourly history.",
    )
    return parser


def main() -> None:
    parser = build_parser()
    args = parser.parse_args()

    try:
        if args.source_export:
            df = parse_export_csv(args.source_export)
        elif DEFAULT_EXPORT_PATH.exists():
            df = parse_export_csv(DEFAULT_EXPORT_PATH)
        else:
            df = fetch_api_dataframe()
    except Exception as exc:
        print(str(exc))
        sys.exit(1)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    df.to_csv(args.output, index=False)
    print(f"Saved {len(df)} hourly rows to {args.output}")
    print("Timestamps were normalized to EAT (Africa/Nairobi).")


if __name__ == "__main__":
    main()
