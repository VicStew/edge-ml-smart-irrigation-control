import requests
import pandas as pd
import sys

url = "https://archive-api.open-meteo.com/v1/archive"
params = {
    "latitude": -0.4167,
    "longitude": 36.9500,
    "start_date": "2022-01-01",
    "end_date": "2025-12-31",
    "daily": "temperature_2m_max,temperature_2m_min,precipitation_sum,et0_fao_evapotranspiration,shortwave_radiation_sum",
    "hourly": "soil_moisture_0_to_7cm",
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
    
    # Process hourly soil moisture
    df_hourly['time'] = pd.to_datetime(df_hourly['time'])
    df_hourly['date'] = df_hourly['time'].dt.strftime('%Y-%m-%d')
    daily_soil = df_hourly.groupby('date')['soil_moisture_0_to_7cm'].mean().reset_index()
    daily_soil.rename(columns={'date': 'time'}, inplace=True)
    
    # Merge daily with aggregated hourly
    df_merged = pd.merge(df_daily, daily_soil, on='time', how='inner')
    
    df_merged.to_csv("historical_weather_data.csv", index=False)
    print("Data fetched and saved to historical_weather_data.csv successfully!")
    print(f"Total rows: {len(df_merged)}")
else:
    print(f"Failed to fetch: {response.status_code}")
    print(response.text)
    sys.exit(1)
