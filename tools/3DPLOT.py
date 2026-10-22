import pandas as pd
import plotly.express as px
import questionary
from pathlib import Path

# --- 1. WORKSPACE SCANNEN ---
runs_dir = Path("runs")
if not runs_dir.exists():
    print("Fehler: Der Ordner 'runs' existiert nicht.")
    exit()

scan_folders = [f.name for f in runs_dir.iterdir() if f.is_dir()]
scan_folders.sort(reverse=True)

selected_folder = questionary.select(
    "Wähle einen Scan-Ordner für den 3D Plot:",
    choices=scan_folders
).ask()

if selected_folder is None:
    exit()

csv_pfad = runs_dir / selected_folder / "results.csv"
df = pd.read_csv(csv_pfad)

# --- 2. SPALTEN AUTOMATISCH ERKENNEN ---
metrics = ['NetProfit', 'MaxDrawdown', 'Trades', 'WinRate', 'Sharpe', 'sortino', 'mc_drawdown']
available_metrics = [m for m in metrics if m in df.columns]
parameters = [col for col in df.columns if col not in metrics]

if len(parameters) < 3:
    print(f"Fehler: Ein 3D-Plot benötigt mindestens 3 Parameter im Grid-Scan. Gefunden: {len(parameters)}")
    exit()

# Auto-Mapping der Achsen (nimmt einfach die ersten verfügbaren Spalten)
x_val = parameters[0]
y_val = parameters[1]
z_val = parameters[2]
color_val = "Sharpe" if "Sharpe" in available_metrics else available_metrics[0]
size_val = "Trades" if "Trades" in df.columns else None

print(f"Rendere 3D Landschaft: X={x_val}, Y={y_val}, Z={z_val}, Farbe={color_val}...")

# --- 3. 3D PLOT ZEICHNEN ---
fig = px.scatter_3d(
    df, 
    x=x_val, 
    y=y_val, 
    z=z_val, 
    color=color_val,
    size=size_val,
    # Exakt deine Farbskala: Blau (Schlecht) -> Orange (Mittel) -> Rot (Gut)
    color_continuous_scale=[(0.0, "#1f77b4"), (0.5, "orange"), (1.0, "#d62728")]
)

fig.update_layout(
    template="plotly_dark", 
    title=f"5D Parameter Landscape ({selected_folder})",
    font=dict(color="lightgrey"), # Augenschonendes Hellgrau statt reinem Weiß
    paper_bgcolor="black",        # Tiefschwarzer Hintergrund um den Plot herum
    scene=dict(
        # Tiefschwarzer Hintergrund und graues Grid im eigentlichen 3D-Raum
        xaxis=dict(backgroundcolor="black", gridcolor="#333333", color="lightgrey"),
        yaxis=dict(backgroundcolor="black", gridcolor="#333333", color="lightgrey"),
        zaxis=dict(backgroundcolor="black", gridcolor="#333333", color="lightgrey")
    )
)
fig.show() # Öffnet sofort einen neuen Tab in deinem Browser