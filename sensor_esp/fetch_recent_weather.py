#!/usr/bin/env python3
"""Fetch the latest 24 hourly weather samples for the sensor ESP sketch."""

from __future__ import annotations

import argparse
import csv
from datetime import datetime
from pathlib import Path
import sys
from urllib.parse import urlencode
from urllib.request import urlopen
import json


BASE_DIR = Path(__file__).resolve().parent
DEFAULT_CSV_PATH = BASE_DIR / "recent_weather_24h.csv"
DEFAULT_HEADER_PATH = BASE_DIR / "recent_weather_sample.h"
OPEN_METEO_URL = "https://api.open-meteo.com/v1/forecast"
HOURLY_FIELDS = [
    "temperature_2m",
    "relative_humidity_2m",
    "soil_temperature_0_to_7cm",
    "soil_moisture_0_to_7cm",
    "et0_fao_evapotranspiration",
    "shortwave_radiation",
    "precipitation",
]


def fetch_recent_weather(
    latitude: float,
    longitude: float,
    timezone_name: str,
) -> list[dict[str, float | str]]:
    params = {
        "latitude": latitude,
        "longitude": longitude,
        "hourly": ",".join(HOURLY_FIELDS),
        "past_days": 1,
        "forecast_days": 1,
        "timezone": timezone_name,
    }
    url = f"{OPEN_METEO_URL}?{urlencode(params)}"

    with urlopen(url, timeout=30) as response:
        payload = json.loads(response.read().decode("utf-8"))

    hourly = payload.get("hourly", {})
    times = hourly.get("time", [])
    if not times:
        raise RuntimeError("Open-Meteo returned no hourly weather data.")

    now = datetime.now().replace(minute=0, second=0, microsecond=0)
    rows: list[dict[str, float | str]] = []

    for index, timestamp in enumerate(times):
        sample_time = datetime.fromisoformat(timestamp)
        if sample_time > now:
            continue

        row: dict[str, float | str] = {"time": timestamp}
        for field in HOURLY_FIELDS:
            row[field] = hourly[field][index]
        rows.append(row)

    if len(rows) < 24:
        raise RuntimeError(f"Only {len(rows)} past hourly samples were returned.")

    return rows[-24:]


def write_csv(
    rows: list[dict[str, float | str]],
    csv_path: Path,
) -> None:
    csv_path.parent.mkdir(parents=True, exist_ok=True)
    with csv_path.open("w", newline="") as csv_file:
        writer = csv.DictWriter(csv_file, fieldnames=["time", *HOURLY_FIELDS])
        writer.writeheader()
        writer.writerows(rows)


def format_float(value: object) -> str:
    literal = f"{float(value):.6g}"
    if "." not in literal and "e" not in literal.lower():
        literal = f"{literal}.0"
    return f"{literal}f"


def write_header(
    rows: list[dict[str, float | str]],
    header_path: Path,
) -> None:
    header_path.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "#ifndef RECENT_WEATHER_SAMPLE_H",
        "#define RECENT_WEATHER_SAMPLE_H",
        "",
        "#include <stdint.h>",
        "",
        "typedef struct {",
        "  uint32_t sample_time;",
        "  float temperature_2m;",
        "  float relative_humidity_2m;",
        "  float soil_temperature_0_to_7cm;",
        "  float soil_moisture_0_to_7cm;",
        "  float et0_fao_evapotranspiration;",
        "  float shortwave_radiation;",
        "} recent_weather_sample_t;",
        "",
        f"static const uint8_t RECENT_WEATHER_SAMPLE_COUNT = {len(rows)};",
        "",
        "static const recent_weather_sample_t RECENT_WEATHER_SAMPLES[RECENT_WEATHER_SAMPLE_COUNT] = {",
    ]

    for index, row in enumerate(rows):
        lines.append(
            "  {"
            f"{index * 3600}, "
            f"{format_float(row['temperature_2m'])}, "
            f"{format_float(row['relative_humidity_2m'])}, "
            f"{format_float(row['soil_temperature_0_to_7cm'])}, "
            f"{format_float(row['soil_moisture_0_to_7cm'])}, "
            f"{format_float(row['et0_fao_evapotranspiration'])}, "
            f"{format_float(row['shortwave_radiation'])}"
            "},"
        )

    lines.extend([
        "};",
        "",
        "#endif  // RECENT_WEATHER_SAMPLE_H",
        "",
    ])
    header_path.write_text("\n".join(lines))


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Fetch recent Open-Meteo weather data for the sensor ESP demo."
    )
    parser.add_argument("--latitude", type=float, default=-0.4201)
    parser.add_argument("--longitude", type=float, default=36.9476)
    parser.add_argument("--timezone", default="Africa/Nairobi")
    parser.add_argument("--csv-output", type=Path, default=DEFAULT_CSV_PATH)
    parser.add_argument("--header-output", type=Path, default=DEFAULT_HEADER_PATH)
    return parser


def main() -> None:
    parser = build_parser()
    args = parser.parse_args()

    try:
        rows = fetch_recent_weather(args.latitude, args.longitude, args.timezone)
    except Exception as exc:
        print(f"Failed to fetch recent weather data: {exc}", file=sys.stderr)
        sys.exit(1)

    write_csv(rows, args.csv_output)
    write_header(rows, args.header_output)

    print(f"Saved {len(rows)} hourly samples to {args.csv_output}")
    print(f"Updated Arduino sample header at {args.header_output}")


if __name__ == "__main__":
    main()
