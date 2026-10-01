import pandas as pd
import time
from pathlib import Path
import questionary

# --- 1. DATEIAUSWAHL ---
# Sucht alle CSV-Dateien im aktuellen Verzeichnis
csv_files = list(Path(".").glob("*.csv"))

if not csv_files:
    print("Fehler: Keine CSV-Dateien im aktuellen Verzeichnis gefunden.")
    exit()

input_file = questionary.select(
    "Wähle die rohe Binance L2 CSV-Datei aus:",
    choices=[f.name for f in csv_files]
).ask()

if input_file is None:
    exit()

output_file = input_file.replace(".csv", "_500ms_snapshot.csv")

# --- 2. DATEN LADEN & VERARBEITEN ---
print(f"Lade rohe L2-Daten aus '{input_file}'...")
start_time = time.time()

# Pandas liest die rohe Datei ein
df = pd.read_csv(input_file)

# Wir prüfen, ob die wichtigste Spalte existiert
if 'timestamp' not in df.columns:
    print("Fehler: Die CSV benötigt zwingend eine 'timestamp' Spalte (in Millisekunden).")
    print(f"Gefundene Spalten: {list(df.columns)}")
    exit()

# Wandelt den Unix-Millisekunden-Timestamp in ein echtes Pandas-Zeitformat um
df['datetime'] = pd.to_datetime(df['timestamp'], unit='ms')
df.set_index('datetime', inplace=True)

print("Takte Daten auf fixes 500ms-Intervall...")

# --- 3. DIE KERNLOGIK (RESAMPLING) ---
# .resample('500ms'): Schneidet die Zeit in harte 500ms Blöcke.
# .last(): Nimmt den allerletzten bekannten Orderbuch-Zustand kurz vor Ablauf der 500ms.
# .ffill(): (Forward-Fill) Wenn in einem 500ms Block KEIN Update von Binance kam, 
#           wird der alte Zustand einfach kopiert (das Orderbuch verschwindet ja nicht).
df_resampled = df.resample('500ms').last().ffill()

# Wir holen den Zeitstempel zurück, damit C++ ihn als sauberen int64 lesen kann
df_resampled['timestamp'] = (df_resampled.index.astype('int64') // 10**6)

# Aufräumen: Leere Reihen am Anfang (bevor der erste Tick kam) entfernen
df_resampled.dropna(inplace=True)

# --- 4. SPEICHERN ---
print(f"Speichere komprimierte Snapshot-CSV nach '{output_file}'...")

# Wir speichern die Datei, zwingen den Timestamp als erste Spalte und werfen den internen Index weg
cols = ['timestamp'] + [col for col in df_resampled.columns if col != 'timestamp']
df_resampled = df_resampled[cols]
df_resampled.to_csv(output_file, index=False)

elapsed = time.time() - start_time
print(f"Erfolg! {len(df)} rohe Ticks auf {len(df_resampled)} Snapshots in {elapsed:.2f} Sekunden reduziert.")