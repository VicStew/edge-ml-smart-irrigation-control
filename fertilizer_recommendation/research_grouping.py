import pandas as pd
from sklearn.cluster import KMeans
import numpy as np

# Load datasets
crop_rec_df = pd.read_csv('crop_recommendation.csv')
fert_df = pd.read_csv('crop_soil_dataset.csv')
fert_df.rename(columns={'Temparature': 'Temperature'}, inplace=True)

# 1. Get mean N, P, K for each crop in crop_rec_df
crop_npk = crop_rec_df.groupby('label')[['N', 'P', 'K']].mean().reset_index()
crop_npk['label'] = crop_npk['label'].str.lower()

# 2. What are the crops in fert_df?
fert_crops = fert_df['Crop Type'].str.lower().unique()
print("Fertilizer crops:", fert_crops)

# 3. How many fert crops are in our NPK mean table?
overlap = set(fert_crops).intersection(set(crop_npk['label']))
print("Overlap:", overlap)
print("Missing NPK for fert crops:", set(fert_crops) - overlap)

# 4. If fert_df has Nitrogen, Potassium, Phosphorous, maybe those are the N,P,K requirements for the crop in that row?
fert_mean_npk = fert_df.copy()
fert_mean_npk['Crop Type'] = fert_mean_npk['Crop Type'].str.lower()
fert_mean_npk = fert_mean_npk.groupby('Crop Type')[['Nitrogen', 'Phosphorous', 'Potassium']].mean().reset_index()
print("\nFertilizer dataset N, P, K means by crop:")
print(fert_mean_npk)

