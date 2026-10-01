import os
from pathlib import Path
import pandas as pd
import seaborn as sns
import matplotlib
matplotlib.use('Qt5Agg')  # Erzwingt das Fenster-Backend
import matplotlib.pyplot as plt
from matplotlib.colors import LinearSegmentedColormap
import questionary

# --- 1. WORKSPACE SCANNEN ---
runs_dir = Path("runs")

if not runs_dir.exists():
    print("Fehler: Der Ordner 'runs' existiert nicht.")
    exit()

scan_folders = [f.name for f in runs_dir.iterdir() if f.is_dir()]
scan_folders.sort(reverse=True)

if not scan_folders:
    print("Keine Scan-Ordner gefunden.")
    exit()

selected_folder = questionary.select(
    "Wähle einen Scan-Ordner für die Auswertung:",
    choices=scan_folders
).ask()

if selected_folder is None:
    exit()

csv_pfad = runs_dir / selected_folder / "results.csv"

if not csv_pfad.exists():
    print(f"Fehler: In {selected_folder} liegt keine 'results.csv'.")
    exit()

df = pd.read_csv(csv_pfad)

# --- 2. DYNAMISCHE SPALTEN-ERKENNUNG ---
metrics = ['NetProfit', 'MaxDrawdown', 'Trades', 'WinRate', 'Sharpe', 'sortino', 'mc_drawdown']
available_metrics = [m for m in metrics if m in df.columns]
parameters = [col for col in df.columns if col not in metrics]

if not parameters:
    print("Fehler: Keine Parameter in der CSV gefunden.")
    exit()

# --- 3. INTERAKTIVE AUSWAHL DER ACHSEN ---
ziel_metrik = questionary.select(
    "Welche Metrik soll die Farbe (Z-Achse) bestimmen?",
    choices=available_metrics
).ask()

x_achse = questionary.select(
    "Welcher Parameter soll auf die X-Achse?",
    choices=parameters
).ask()

y_choices = ["-> KEINE (1D Linien-Chart zeichnen)"] + [p for p in parameters if p != x_achse]
y_selection = questionary.select(
    "Welcher Parameter soll auf die Y-Achse?",
    choices=y_choices
).ask()

if y_selection is None or x_achse is None or ziel_metrik is None:
    exit()

y_achse = None if y_selection.startswith("->") else y_selection

print(f"Lade Daten... ({len(df)} Iterationen)")

# --- 4. DATEN PLOTTEN (DARK MODE) ---
# Globales dunkles Theme setzen
plt.style.use('dark_background')
plt.rcParams.update({
    "figure.facecolor": "black",
    "axes.facecolor": "black",
    "text.color": "lightgrey",
    "axes.labelcolor": "lightgrey",
    "xtick.color": "lightgrey",
    "ytick.color": "lightgrey",
    "grid.color": "#333333" # Dezent für den 1D Chart
})

# Deine eigene Farbskala: Blau (niedrige Werte) -> Orange -> Rot (hohe Werte)
# Wir nehmen etwas weichere Hex-Farben (#1f77b4 = schönes Blau, #d62728 = kräftiges Rot), 
# damit es auf dem schwarzen Hintergrund nicht in den Augen brennt.
custom_cmap = LinearSegmentedColormap.from_list("BlueOrangeRed", ["#1f77b4", "orange", "#d62728"])

plt.figure(figsize=(12, 8))

if y_achse is None:
    # --- 1D LINE CHART ---
    df_grouped = df.groupby(x_achse)[ziel_metrik].mean().reset_index()
    
    sns.lineplot(
        data=df_grouped, 
        x=x_achse, 
        y=ziel_metrik, 
        marker="o",        
        linewidth=2.5,
        color="#d62728" # Passt sich ans rote "Gut"-Schema an
    )
    plt.title(f"1D Parameter Scan: {ziel_metrik} über {x_achse}\n(Andere Parameter wurden gemittelt)", fontsize=14, pad=15)
    plt.grid(True, linestyle="--", alpha=0.7)
    
else:
    # --- 2D HEATMAP ---
    heatmap_daten = df.pivot_table(
        index=y_achse, 
        columns=x_achse, 
        values=ziel_metrik, 
        aggfunc='mean'
    )
    
    sns.heatmap(
        heatmap_daten, 
        annot=True,
        fmt=".2f",
        cmap=custom_cmap,    # Nutzt ab sofort deine neue Blau-Orange-Rot Skala
        cbar_kws={'label': ziel_metrik}
    )
    
    zusatz = "\n(Weitere Parameter wurden in die Kacheln gemittelt)" if len(parameters) > 2 else ""
    plt.title(f"2D Heatmap: {ziel_metrik} ({x_achse} vs {y_achse}){zusatz}", fontsize=14, pad=15)
    plt.gca().invert_yaxis()

plt.xlabel(x_achse, fontsize=12)
if y_achse is not None:
    plt.ylabel(y_achse, fontsize=12)
plt.tight_layout()
plt.show()