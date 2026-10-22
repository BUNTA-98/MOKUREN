import pandas as pd
import numpy as np
import time
import sys
import os

# Definition des exakten C++ Speicherlayouts (200 Bytes pro Snapshot)
# < bedeutet Little-Endian (Standard für x86 CPUs)
# f8 = 8 Byte double, i8 = 8 Byte int64
C_STRUCT_DTYPE = np.dtype([
    ('timestamp', '<i8'),      
    ('best_bid_price', '<f8'), 
    ('best_bid_qty', '<f8'),   
    ('best_ask_price', '<f8'), 
    ('best_ask_qty', '<f8'),   
    ('bids', '<f8', 10),       # Array aus 10 doubles: [p0, q0, p1, q1, p2, q2, p3, q3, p4, q4]
    ('asks', '<f8', 10)        # Array aus 10 doubles: [p0, q0, p1, q1, p2, q2, p3, q3, p4, q4]
])

def convert_l2_to_binary(input_csv, interval_ms=500):
    input_dir = os.path.dirname(input_csv)
    output_bin = os.path.join(input_dir, "L2.bin") if input_dir else "L2.bin"
    
    print(f"Lese Rohdaten: {input_csv}...")
    start_time = time.time()
    
    # CSV einlesen (wirft Warnungen, falls Spalten fehlen, nimmt dann NaN)
    df = pd.read_csv(input_csv)
    initial_rows = len(df)
    
    print(f"[{initial_rows:,}] Ticks geladen. Starte Resampling auf {interval_ms}ms...")

    # Zeit normalisieren und Duplikate filtern
    df['transaction_time'] = (df['transaction_time'] // interval_ms) * interval_ms
    df = df.drop_duplicates(subset=['transaction_time'], keep='last')
    
    compressed_rows = len(df)
    
    # Numpy Array mit unserem C-Struct Layout initialisieren
    bin_data = np.zeros(compressed_rows, dtype=C_STRUCT_DTYPE)
    
    # Legacy Level 1 & Timestamp mappen
    bin_data['timestamp'] = df['transaction_time'].astype(np.int64)
    bin_data['best_bid_price'] = df['best_bid_price'].astype(np.float64)
    bin_data['best_bid_qty'] = df['best_bid_qty'].astype(np.float64)
    bin_data['best_ask_price'] = df['best_ask_price'].astype(np.float64)
    bin_data['best_ask_qty'] = df['best_ask_qty'].astype(np.float64)
    
    # Schleife über die 5 Orderbuch-Tiefen (0 = best bid, 1 = zweites level, etc.)
    for i in range(5):
        # Namensschema der Spalten in der CSV abfragen
        # Passe diese Strings an, falls cryptohftdata.com die Spalten anders nennt!
        if i == 0:
            bid_p_col, bid_q_col = 'best_bid_price', 'best_bid_qty'
            ask_p_col, ask_q_col = 'best_ask_price', 'best_ask_qty'
        else:
            bid_p_col, bid_q_col = f'bid{i+1}_price', f'bid{i+1}_qty'
            ask_p_col, ask_q_col = f'ask{i+1}_price', f'ask{i+1}_qty'
            
        # Wenn die Spalte existiert, in den korrekten Array-Index schreiben
        # Price ist an geraden Indizes (0, 2, 4...), Qty an ungeraden (1, 3, 5...)
        if bid_p_col in df.columns:
            bin_data['bids'][:, i*2] = df[bid_p_col].fillna(0.0)
            bin_data['bids'][:, i*2 + 1] = df[bid_q_col].fillna(0.0)
            
        if ask_p_col in df.columns:
            bin_data['asks'][:, i*2] = df[ask_p_col].fillna(0.0)
            bin_data['asks'][:, i*2 + 1] = df[ask_q_col].fillna(0.0)

    # Direkt als binären C-Block wegschreiben
    bin_data.tofile(output_bin)
    
    duration = time.time() - start_time
    compression_rate = (1 - (compressed_rows / initial_rows)) * 100
    file_size_mb = (compressed_rows * 200) / (1024*1024)
    
    print("\n--- KONVERTIERUNG ABGESCHLOSSEN ---")
    print(f"Dauer:         {duration:.2f} Sekunden")
    print(f"Ticks neu:     {compressed_rows:,} (Komprimiert um {compression_rate:.1f}%)")
    print(f"Dateigröße:    {file_size_mb:.2f} MB")
    print(f"Output:        {output_bin}")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Nutzung: python L2CONV.py <input.csv>")
    else:
        convert_l2_to_binary(sys.argv[1], interval_ms=500)