import pandas as pd
import numpy as np
from sklearn.ensemble import RandomForestRegressor, RandomForestClassifier
from sklearn.cluster import KMeans
from sklearn.model_selection import train_test_split
from sklearn.metrics import accuracy_score
from sklearn.preprocessing import LabelEncoder
import warnings
warnings.filterwarnings('ignore')

crop_rec_df = pd.read_csv('crop_recommendation.csv')
crop_train_df = pd.read_csv('Crop Recommendation Dataset(Training_Data).csv')
fert_df = pd.read_csv('crop_soil_dataset.csv')

crop_train_df.rename(columns={'Temperature':'temperature', 'Humidity':'humidity', 'pH':'ph', 'Rainfall':'rainfall', 'Label':'label'}, inplace=True)
crop_rec_df['label'] = crop_rec_df['label'].str.lower()
crop_train_df['label'] = crop_train_df['label'].str.lower()

features = ['temperature', 'humidity', 'ph', 'rainfall']
targets = ['N', 'P', 'K']

X_train_imp = crop_rec_df[features]
y_train_imp = crop_rec_df[targets]

# Strong regressor
regressor = RandomForestRegressor(n_estimators=100, random_state=42)
regressor.fit(X_train_imp, y_train_imp)

predicted_npk = regressor.predict(crop_train_df[features])
crop_train_df['N'] = predicted_npk[:, 0]
crop_train_df['P'] = predicted_npk[:, 1]
crop_train_df['K'] = predicted_npk[:, 2]
crop_train_df = crop_train_df[['N', 'P', 'K', 'temperature', 'humidity', 'ph', 'rainfall', 'label']]

combined_crop_df = pd.concat([crop_rec_df, crop_train_df], ignore_index=True)

for n_estimators, max_depth in [(15, 8), (30, 12), (50, 15), (100, None)]:
    X_crop = combined_crop_df[['N', 'P', 'K', 'temperature', 'humidity', 'ph', 'rainfall']]
    y_crop = combined_crop_df['label']
    X_train_c, X_test_c, y_train_c, y_test_c = train_test_split(X_crop, y_crop, test_size=0.2, random_state=42)
    crop_clf = RandomForestClassifier(n_estimators=n_estimators, max_depth=max_depth, random_state=42)
    crop_clf.fit(X_train_c, y_train_c)
    y_pred_c = crop_clf.predict(X_test_c)
    print(f"Crop Accuracy (n={n_estimators}, d={max_depth}): {accuracy_score(y_test_c, y_pred_c):.4f}")

# Fertilizer test with clustering
for n_clusters in [5, 8, 11]:
    crop_npk_means = combined_crop_df.groupby('label')[['N', 'P', 'K']].mean().reset_index()
    kmeans = KMeans(n_clusters=n_clusters, random_state=42)
    crop_npk_means['Crop_NPK_Group'] = kmeans.fit_predict(crop_npk_means[['N', 'P', 'K']])
    crop_to_group_dict = dict(zip(crop_npk_means['label'], crop_npk_means['Crop_NPK_Group']))

    fert_df_temp = fert_df.copy()
    fert_df_temp.rename(columns={'Temparature': 'Temperature'}, inplace=True)
    fert_df_temp['Crop Type'] = fert_df_temp['Crop Type'].str.lower()

    manual_mapping = {'paddy': 'rice', 'ground nuts': 'groundnut', 'wheat': 'maize', 'barley': 'maize', 'millets': 'maize', 'tobacco': 'cotton', 'oil seeds': 'groundnut', 'sugarcane': 'sugarcane', 'cotton': 'cotton', 'maize': 'maize', 'pulses': 'pulses'}
    fert_df_temp['Crop_NPK_Group'] = fert_df_temp['Crop Type'].apply(lambda c: crop_to_group_dict.get(manual_mapping.get(c, c), 0))
    le_soil = LabelEncoder()
    fert_df_temp['Soil Type'] = le_soil.fit_transform(fert_df_temp['Soil Type'])

    X_fert = fert_df_temp[['Temperature', 'Humidity', 'Moisture', 'Soil Type', 'Crop_NPK_Group', 'Nitrogen', 'Potassium', 'Phosphorous']]
    y_fert = fert_df_temp['Fertilizer Name']
    X_train_f, X_test_f, y_train_f, y_test_f = train_test_split(X_fert, y_fert, test_size=0.2, random_state=42)

    for n_estimators, max_depth in [(15, 8), (30, 12), (50, 15), (100, None)]:
        fert_clf = RandomForestClassifier(n_estimators=n_estimators, max_depth=max_depth, random_state=42)
        fert_clf.fit(X_train_f, y_train_f)
        y_pred_f = fert_clf.predict(X_test_f)
        print(f"Fert Accuracy (clusters={n_clusters}, n={n_estimators}, d={max_depth}): {accuracy_score(y_test_f, y_pred_f):.4f}")

# Baseline fert accuracy (without grouping)
fert_df_base = fert_df.copy()
fert_df_base.rename(columns={'Temparature': 'Temperature'}, inplace=True)
le_soil_base = LabelEncoder()
fert_df_base['Soil Type'] = le_soil_base.fit_transform(fert_df_base['Soil Type'])
le_crop_base = LabelEncoder()
fert_df_base['Crop Type'] = le_crop_base.fit_transform(fert_df_base['Crop Type'])
X_fert_base = fert_df_base[['Temperature', 'Humidity', 'Moisture', 'Soil Type', 'Crop Type', 'Nitrogen', 'Potassium', 'Phosphorous']]
y_fert_base = fert_df_base['Fertilizer Name']
X_train_fb, X_test_fb, y_train_fb, y_test_fb = train_test_split(X_fert_base, y_fert_base, test_size=0.2, random_state=42)
clf_base = RandomForestClassifier(n_estimators=100, random_state=42)
clf_base.fit(X_train_fb, y_train_fb)
print(f"Baseline Fert Accuracy (no groups, n=100, d=None): {accuracy_score(y_test_fb, clf_base.predict(X_test_fb)):.4f}")
