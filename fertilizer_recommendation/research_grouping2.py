import pandas as pd

# Combined crop datasets from the notebook
crop_rec_df = pd.read_csv('crop_recommendation.csv')
crop_rec_df['label'] = crop_rec_df['label'].str.lower()
crop_train_df = pd.read_csv('Crop Recommendation Dataset(Training_Data).csv')
crop_train_df.rename(columns={'Label':'label'}, inplace=True)
crop_train_df['label'] = crop_train_df['label'].str.lower()

all_crops = set(crop_rec_df['label']).union(set(crop_train_df['label']))

fert_df = pd.read_csv('crop_soil_dataset.csv')
fert_crops = set(fert_df['Crop Type'].str.lower().unique())

print("All crop models crops count:", len(all_crops))
print("Fert crops count:", len(fert_crops))
print("Fert crops not in all crops:", fert_crops - all_crops)
