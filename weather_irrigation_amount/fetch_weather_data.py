from pathlib import Path
import sys

import pandas as pd
import requests

BASE_DIR = Path(__file__).resolve().parent
OUTPUT_CSV = BASE_DIR / "historical_weather_data.csv"
url = "https://archive-api.open-meteo.com/v1/archive"
params = {
    "latitude": -0.4167,
    "longitude": 36.9500,
    "start_date": "2022-01-01",
    "end_date": "2025-12-31",
    "daily": "temperature_2m_max,temperature_2m_min,precipitation_sum,et0_fao_evapotranspiration,shortwave_radiation_sum",
    "hourly": "soil_moisture_0_to_7cm,relative_humidity_2m,vapor_pressure_deficit,wind_speed_10m",
    "timezone": "Africa/Nairobi"
}

print("Fetching historical weather data for Nyeri County...")
response = requests.get(url, params=params)
if response.status_code == 200:
    data = response.json()
    
    if "daily" not in data or "hourly" not in data:
        print("Required data not found in response.")
        sys.exit(1)
        
    df_daily = pd.DataFrame(data['daily'])
    df_hourly = pd.DataFrame(data['hourly'])
    
    # Process hourly data
    df_hourly['time'] = pd.to_datetime(df_hourly['time'])
    df_hourly['date'] = df_hourly['time'].dt.strftime('%Y-%m-%d')
    daily_aggregated = df_hourly.groupby('date').mean(numeric_only=True).reset_index()
    daily_aggregated.rename(columns={'date': 'time'}, inplace=True)
    
    # Merge daily with aggregated hourly
    df_merged = pd.merge(df_daily, daily_aggregated, on='time', how='inner')

    # Keep source field names and expose clear aliases for the MLP outputs.
    df_merged["precipitation"] = df_merged["precipitation_sum"]
    df_merged["evapotranspiration"] = df_merged["et0_fao_evapotranspiration"]

    ordered_columns = [
        "time",
        "temperature_2m_max",
        "temperature_2m_min",
        "shortwave_radiation_sum",
        "soil_moisture_0_to_7cm",
        "relative_humidity_2m",
        "vapor_pressure_deficit",
        "wind_speed_10m",
        "precipitation_sum",
        "et0_fao_evapotranspiration",
        "precipitation",
        "evapotranspiration",
    ]
    df_merged = df_merged[ordered_columns]

    df_merged.to_csv(OUTPUT_CSV, index=False)
    print(f"Data fetched and saved to {OUTPUT_CSV.name} successfully!")
    print(f"Total rows: {len(df_merged)}")
else:
    print(f"Failed to fetch: {response.status_code}")
    print(response.text)
    sys.exit(1)
