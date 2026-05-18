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
LOCAL_TIMEZONE_NAME = "Africa/Nairobi"
URL = "https://archive-api.open-meteo.com/v1/archive"
PARAMS = {
	"latitude": -0.4201,
	"longitude": 36.9476,
	"start_date": "2015-01-01",
	"end_date": "2025-12-31",
	"hourly": ["temperature_2m", "relative_humidity_2m", "soil_temperature_0_to_7cm", "precipitation", "et0_fao_evapotranspiration", "shortwave_radiation"],
	"timezone": "Africa/Nairobi",
}

INPUT_RENAME_MAP = {
    "precipitation": "precipitation_mm",
    "precipitation (mm)": "precipitation_mm",
    "et0_fao_evapotranspiration": "et0_fao_evapotranspiration_mm",
    "et0_fao_evapotranspiration (mm)": "et0_fao_evapotranspiration_mm",
}


def parse_export_csv(export_path: Path) -> pd.DataFrame:
    raw_text = export_path.read_text()
    parts = raw_text.split("\n\n", maxsplit=1)
    if len(parts) != 2:
        raise ValueError("Unexpected Open-Meteo export format: missing metadata separator.")

    metadata_df = pd.read_csv(StringIO(parts[0]))
    data_df = pd.read_csv(StringIO(parts[1]))
    timezone_name = str(metadata_df.loc[0, "timezone"]).strip()
    return normalize_hourly_dataframe(data_df, timezone_name=timezone_name)


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
    return normalize_hourly_dataframe(data_df, timezone_name=LOCAL_TIMEZONE_NAME)

def normalize_hourly_dataframe(data_df: pd.DataFrame, timezone_name: str) -> pd.DataFrame:
    df = data_df.rename(columns=INPUT_RENAME_MAP).copy()
    required_columns = {
        "time",
        "temperature_2m",
        "relative_humidity_2m",
        "soil_temperature_0_to_7cm",
        "shortwave_radiation",
        "et0_fao_evapotranspiration_mm",
        "precipitation_mm",
    }
    missing_columns = sorted(required_columns.difference(df.columns))
    if missing_columns:
        raise ValueError(f"Hourly weather data is missing expected columns: {missing_columns}")

    timestamps = pd.to_datetime(df["time"])
    
    # Open-Meteo returns time in local time when timezone is set
    df["time"] = timestamps.dt.strftime("%Y-%m-%dT%H:%M:%S")
    df["precipitation_mm"] = df["precipitation_mm"].clip(lower=0)

    ordered_columns = [
        "time",
        "temperature_2m",
        "relative_humidity_2m",
        "soil_temperature_0_to_7cm",
        "shortwave_radiation",
        "et0_fao_evapotranspiration_mm",
        "precipitation_mm",
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
        help="Destination CSV for local-time hourly history.",
    )
    return parser


def main() -> None:
    parser = build_parser()
    args = parser.parse_args()

    try:
        if args.source_export:
            df = parse_export_csv(args.source_export)
        else:
            df = fetch_api_dataframe()
    except Exception as exc:
        print(str(exc))
        sys.exit(1)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    df.to_csv(args.output, index=False)
    print(f"Saved {len(df)} hourly rows to {args.output}")
    print(f"Timestamps were normalized to local time ({LOCAL_TIMEZONE_NAME}).")


if __name__ == "__main__":
    main()
