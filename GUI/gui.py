import pandas as pd
import matplotlib.pyplot as plt
from matplotlib.ticker import MaxNLocator

# Load the CSV file into a Pandas DataFrame
df = pd.read_csv('Proxy Statistics.csv')

# Prepend '00:' to 'Time Elapsed' to convert it into 'HH:MM:SS' format
df['Time Elapsed'] = '00:' + df['Time Elapsed']
df['Time Elapsed'] = pd.to_timedelta(df['Time Elapsed'])
min_time = df['Time Elapsed'].min()
df['Time Elapsed'] = df['Time Elapsed'] - min_time

# Convert 'Time Elapsed' to the format MM:SS for plotting
df['Time Elapsed'] = df['Time Elapsed'].dt.total_seconds().apply(lambda x: f"{int(x // 60):02d}:{int(x % 60):02d}")

# Setting the new 'Time Elapsed' as index
df.set_index('Time Elapsed', inplace=True)

# Normalize each data column to start from 0
for column in df.columns:
    df[column] -= df[column].iloc[0]

# Apply an offset to all points after the first one
offset = 0.1  # Define an offset value
for i, column in enumerate(df.columns):
    # Apply an offset based on the column index, skipping the first point
    df.loc[df.index[1:], column] += i * offset

# Plot each category over time with lines, dots, and an offset
plt.figure(figsize=(10, 5))

# Using loop to plot each column with an offset
for i, column in enumerate(df.columns):
    plt.plot(df.index, df[column], label=column, marker='o', linestyle='-', markersize=5, alpha=0.5)

# Formatting the plot
plt.xlabel('Time Elapsed (Minutes:Seconds)')
plt.ylabel('Count')
plt.title('Proxy Statistics Over Time')

# Use MaxNLocator to ensure y-axis has integer ticks
plt.gca().yaxis.set_major_locator(MaxNLocator(integer=True))

plt.legend()
plt.tight_layout()

plt.xticks(rotation=45)  # Rotate x-ticks for better readability
plt.show()

