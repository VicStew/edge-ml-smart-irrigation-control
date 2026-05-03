import pandas as pd
from sklearn.ensemble import RandomForestClassifier
from sklearn.model_selection import train_test_split
from sklearn.preprocessing import LabelEncoder

df = pd.read_csv('crop_soil_dataset.csv')
print("Fertilizer Names distribution:")
print(df['Fertilizer Name'].value_counts())

df.rename(columns={'Temparature': 'Temperature'}, inplace=True)
le1 = LabelEncoder()
df['Soil Type'] = le1.fit_transform(df['Soil Type'])
le2 = LabelEncoder()
df['Crop Type'] = le2.fit_transform(df['Crop Type'])
le3 = LabelEncoder()
df['Fertilizer Name'] = le3.fit_transform(df['Fertilizer Name'])

X = df[['Temperature', 'Humidity', 'Moisture', 'Soil Type', 'Crop Type', 'Nitrogen', 'Potassium', 'Phosphorous']]
y = df['Fertilizer Name']

X_train, X_test, y_train, y_test = train_test_split(X, y, test_size=0.2, random_state=42)
clf = RandomForestClassifier(n_estimators=100, random_state=42)
clf.fit(X_train, y_train)

from sklearn.metrics import accuracy_score
print("Accuracy:", accuracy_score(y_test, clf.predict(X_test)))
print("Train Accuracy:", accuracy_score(y_train, clf.predict(X_train)))

importances = clf.feature_importances_
for name, imp in zip(X.columns, importances):
    print(f"{name}: {imp:.4f}")
