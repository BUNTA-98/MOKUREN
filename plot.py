import pandas as pd
import matplotlib.pyplot as plt
from datetime import datetime

# Lade die CSV aus deinem Projekt-Ordner
file_path = "runs.csv"

# Lese die Daten ein
try:
    df = pd.read_csv(file_path)
except FileNotFoundError:
    print(f"Fehler: {file_path} nicht gefunden. Starte zuerst den Backtest!")
    exit()

# Wir filtern nur den absolut besten Run (Rank 1)
df = df[df['Rank'] == 1].copy()

# UNIX Timestamps in lesbare Daten umwandeln
df['EntryTime'] = pd.to_datetime(df['EntryTime'], unit='ms')

# Berechne die kumulierte Equity Curve (Startkapital 10000)
df['Equity'] = 10000 + df['NetProfit'].cumsum()

# Finde den maximalen Drawdown (Peak to Trough)
df['Peak'] = df['Equity'].cummax()
df['Drawdown'] = (df['Equity'] - df['Peak']) / df['Peak'] * 100
max_drawdown = df['Drawdown'].min()
total_profit = df['NetProfit'].sum()
win_rate = (len(df[df['NetProfit'] > 0]) / len(df)) * 100

# Erstelle den Plot
fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(12, 8), gridspec_kw={'height_ratios': [3, 1]})
fig.suptitle(f"Backtest Performance: Rank 1 (Profit: {total_profit:.2f}$ | Winrate: {win_rate:.1f}%)", fontsize=16)

# Plot 1: Equity Curve
ax1.plot(df['EntryTime'], df['Equity'], label="Account Balance", color='blue', linewidth=2)
ax1.set_ylabel("Account Size (USDT)", fontsize=12)
ax1.grid(True, linestyle='--', alpha=0.6)
ax1.legend()

# Markiere Wins (Grün) und Losses (Rot) auf der Curve
wins = df[df['NetProfit'] > 0]
losses = df[df['NetProfit'] < 0]
ax1.scatter(wins['EntryTime'], wins['Equity'], color='green', marker='^', label='Win', s=50, zorder=5)
ax1.scatter(losses['EntryTime'], losses['Equity'], color='red', marker='v', label='Loss', s=50, zorder=5)

# Plot 2: Drawdown
ax2.fill_between(df['EntryTime'], df['Drawdown'], 0, color='red', alpha=0.3)
ax2.plot(df['EntryTime'], df['Drawdown'], color='red', linewidth=1)
ax2.set_ylabel("Drawdown (%)", fontsize=12)
ax2.set_xlabel("Date", fontsize=12)
ax2.grid(True, linestyle='--', alpha=0.6)
ax2.set_title(f"Max Drawdown: {max_drawdown:.2f}%", fontsize=10, loc='left')

plt.tight_layout()
plt.show()
