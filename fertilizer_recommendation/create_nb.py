import nbformat as nbf

nb = nbf.v4.new_notebook()

nb['cells'] = [
    nbf.v4.new_markdown_cell("# Crop and Fertilizer Recommendation Models (ESP32 Optimized)\n\nThis notebook merges multiple datasets, imputes missing values, clusters crops by NPK requirements, and trains lightweight ML models for ESP32 deployment."),
    
    nbf.v4.new_markdown_cell("## 1. Import Libraries"),
    nbf.v4.new_code_cell("import pandas as pd\nimport numpy as np\nfrom sklearn.ensemble import RandomForestRegressor, RandomForestClassifier\nfrom sklearn.cluster import KMeans\nfrom sklearn.model_selection import train_test_split\nfrom sklearn.metrics import accuracy_score\nfrom sklearn.preprocessing import LabelEncoder\nimport joblib\nimport warnings\nwarnings.filterwarnings('ignore')"),

    nbf.v4.new_markdown_cell("## 2. Load Datasets"),
    nbf.v4.new_code_cell("crop_rec_df = pd.read_csv('crop_recommendation.csv')\ncrop_train_df = pd.read_csv('Crop Recommendation Dataset(Training_Data).csv')\nfert_df = pd.read_csv('crop_soil_dataset.csv')\n\nprint('Crop Recommendation shape:', crop_rec_df.shape)\nprint('Training Data shape:', crop_train_df.shape)\nprint('Fertilizer Data shape:', fert_df.shape)"),

    nbf.v4.new_markdown_cell("## 3. Data Imputation for Training Data\nThe Training Data is missing `N`, `P`, and `K`. We will use a RandomForestRegressor trained on the crop recommendation dataset to predict and impute these values."),
    nbf.v4.new_code_cell("""# Standardize column names
crop_train_df.rename(columns={'Temperature':'temperature', 'Humidity':'humidity', 'pH':'ph', 'Rainfall':'rainfall', 'Label':'label'}, inplace=True)
crop_rec_df['label'] = crop_rec_df['label'].str.lower()
crop_train_df['label'] = crop_train_df['label'].str.lower()

# Train Regressor to predict N, P, K
features = ['temperature', 'humidity', 'ph', 'rainfall']
targets = ['N', 'P', 'K']

X_train_imp = crop_rec_df[features]
y_train_imp = crop_rec_df[targets]

regressor = RandomForestRegressor(n_estimators=50, random_state=42)
regressor.fit(X_train_imp, y_train_imp)

# Predict missing values
predicted_npk = regressor.predict(crop_train_df[features])
crop_train_df['N'] = predicted_npk[:, 0]
crop_train_df['P'] = predicted_npk[:, 1]
crop_train_df['K'] = predicted_npk[:, 2]

# Reorder columns to match
crop_train_df = crop_train_df[['N', 'P', 'K', 'temperature', 'humidity', 'ph', 'rainfall', 'label']]"""),

    nbf.v4.new_markdown_cell("## 4. Merge Crop Datasets & NPK Clustering\nWe will cluster the 73 unique crops into 5 NPK groups. This reduces feature cardinality and bridges the gap between the crop dataset and the fertilizer dataset."),
    nbf.v4.new_code_cell("""combined_crop_df = pd.concat([crop_rec_df, crop_train_df], ignore_index=True)
print("Combined shape:", combined_crop_df.shape)

# Calculate mean N, P, K for each crop
crop_npk_means = combined_crop_df.groupby('label')[['N', 'P', 'K']].mean().reset_index()

# Cluster into 5 groups
kmeans = KMeans(n_clusters=5, random_state=42)
crop_npk_means['Crop_NPK_Group'] = kmeans.fit_predict(crop_npk_means[['N', 'P', 'K']])

# Create a mapping dictionary: Crop Label -> NPK Group
crop_to_group_dict = dict(zip(crop_npk_means['label'], crop_npk_means['Crop_NPK_Group']))

print("Clustering complete. Groups assigned:", crop_npk_means['Crop_NPK_Group'].unique())
print(crop_npk_means.head(10))"""),

    nbf.v4.new_markdown_cell("## 5. Train Crop Recommendation Model\nUsing lightweight parameters for ESP32."),
    nbf.v4.new_code_cell("""X_crop = combined_crop_df[['N', 'P', 'K', 'temperature', 'humidity', 'ph', 'rainfall']]
y_crop = combined_crop_df['label']

X_train_c, X_test_c, y_train_c, y_test_c = train_test_split(X_crop, y_crop, test_size=0.2, random_state=42)

crop_clf = RandomForestClassifier(n_estimators=50, max_depth=12, random_state=42)
crop_clf.fit(X_train_c, y_train_c)

y_pred_c = crop_clf.predict(X_test_c)
print(f"Crop Recommendation Accuracy: {accuracy_score(y_test_c, y_pred_c):.4f}")"""),

    nbf.v4.new_markdown_cell("## 6. Process Fertilizer Dataset\nMap the 11 crops in the fertilizer dataset to their corresponding NPK Group from the clustering."),
    nbf.v4.new_code_cell("""# Clean column names
fert_df.rename(columns={'Temparature': 'Temperature'}, inplace=True)
fert_df['Crop Type'] = fert_df['Crop Type'].str.lower()

# Manual mapping logic for crops that were not in the main crop dataset
manual_mapping = {
    'paddy': 'rice',
    'ground nuts': 'groundnut',
    'wheat': 'maize',
    'barley': 'maize',
    'millets': 'maize',
    'tobacco': 'cotton',
    'oil seeds': 'groundnut',
    'sugarcane': 'sugarcane',
    'cotton': 'cotton',
    'maize': 'maize',
    'pulses': 'pulses'
}

# Apply mapping to get the cluster ID
def get_crop_group(crop_name):
    mapped_name = manual_mapping.get(crop_name, crop_name)
    return crop_to_group_dict.get(mapped_name, 0) # default to group 0 if missing

fert_df['Crop_NPK_Group'] = fert_df['Crop Type'].apply(get_crop_group)

# Encode Soil Type
le_soil = LabelEncoder()
fert_df['Soil Type'] = le_soil.fit_transform(fert_df['Soil Type'])
"""),

    nbf.v4.new_markdown_cell("## 7. Train Fertilizer Recommendation Model\nTrain using `Crop_NPK_Group` instead of `Crop Type`."),
    nbf.v4.new_code_cell("""X_fert = fert_df[['Temperature', 'Humidity', 'Moisture', 'Soil Type', 'Crop_NPK_Group', 'Nitrogen', 'Potassium', 'Phosphorous']]
y_fert = fert_df['Fertilizer Name']

X_train_f, X_test_f, y_train_f, y_test_f = train_test_split(X_fert, y_fert, test_size=0.2, random_state=42)

fert_clf = RandomForestClassifier(n_estimators=50, max_depth=12, random_state=42)
fert_clf.fit(X_train_f, y_train_f)

y_pred_f = fert_clf.predict(X_test_f)
print(f"Fertilizer Recommendation Accuracy: {accuracy_score(y_test_f, y_pred_f):.4f}")"""),

    nbf.v4.new_markdown_cell("## 8. Save Models & Artifacts\nSave everything required for deployment."),
    nbf.v4.new_code_cell("""joblib.dump(crop_clf, 'crop_recommender.joblib')
joblib.dump(fert_clf, 'fertilizer_recommender.joblib')
joblib.dump(le_soil, 'le_soil.joblib')
joblib.dump(kmeans, 'kmeans_clusterer.joblib')
joblib.dump(crop_to_group_dict, 'crop_to_group_dict.joblib')

print("Models and artifacts saved successfully!")""")
]

with open('irrigation_models.ipynb', 'w') as f:
    nbf.write(nb, f)
