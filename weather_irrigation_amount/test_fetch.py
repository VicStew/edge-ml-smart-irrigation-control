import requests
import pandas as pd

url = "https://archive-api.open-meteo.com/v1/archive"
params = {
    "latitude": -0.4167,
    "longitude": 36.9500,
    "start_date": "2025-01-01",
    "end_date": "2025-12-31",
    "daily": "temperature_2m_max,temperature_2m_min,precipitation_sum,et0_fao_evapotranspiration",
    "timezone": "Africa/Nairobi"
}

response = requests.get(url, params=params)
if response.status_code == 200:
    data = response.json()
    daily = data.get('daily', {})
    if daily:
        df = pd.DataFrame(daily)
        print("Data fetched successfully!")
        print(df.head())
        print(f"Total rows: {len(df)}")
else:
    print(f"Failed to fetch: {response.status_code}")
    print(response.text)
