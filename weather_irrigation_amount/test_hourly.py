import requests
import pandas as pd

url = "https://archive-api.open-meteo.com/v1/archive"
params = {
    "latitude": -0.4167,
    "longitude": 36.9500,
    "start_date": "2023-01-01",
    "end_date": "2023-01-05",
    "daily": "temperature_2m_max,temperature_2m_min,precipitation_sum,et0_fao_evapotranspiration,shortwave_radiation_sum",
    "hourly": (
        "precipitation_probability,rain,cloud_cover,et0_fao_evapotranspiration,"
        "soil_temperature_0_to_7cm,soil_moisture_0_to_7cm"
    ),
    "timezone": "Africa/Nairobi"
}

response = requests.get(url, params=params)
data = response.json()

if "daily" in data:
    df_daily = pd.DataFrame(data['daily'])
    print("Daily data columns:", df_daily.columns.tolist())
    
if "hourly" in data:
    df_hourly = pd.DataFrame(data['hourly'])
    print("Hourly data columns:", df_hourly.columns.tolist())
    df_hourly['time'] = pd.to_datetime(df_hourly['time'])
    df_hourly['date'] = df_hourly['time'].dt.date
    df_hourly['rain_mm'] = df_hourly['rain'].clip(lower=0)
    df_hourly['rain_occurrence'] = (df_hourly['rain_mm'] >= 0.1).astype(int)
    daily_summary = df_hourly.groupby('date')[
        [
            'precipitation_probability',
            'rain_mm',
            'cloud_cover',
            'et0_fao_evapotranspiration',
            'soil_temperature_0_to_7cm',
            'soil_moisture_0_to_7cm',
            'rain_occurrence',
        ]
    ].mean().reset_index()
    print("Aggregated hourly rainfall-related signals:")
    print(daily_summary.head())
