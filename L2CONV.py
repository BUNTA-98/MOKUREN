import pandas as pd
import numpy as np
import time
import sys
import os

def convert_l2_to_binary(input_csv, interval_ms=500):
    # Automatischen Output-Pfad generieren (gleicher Ordner, Name: L2.bin)
    input_dir = os.path.dirname(input_csv)
    output_bin = os.path.join(input_dir, "L2.bin") if input_dir else "L2.bin"
    
    print(f"Lese Rohdaten: {input_csv}...")
    start_time = time.time()
    
    columns = ['transaction_time', 'best_bid_price', 'best_bid_qty', 'best_ask_price', 'best_ask_qty']
    df = pd.read_csv(input_csv, usecols=columns)
    
    initial_rows = len(df)
    print(f"[{initial_rows:,}] Ticks geladen. Starte Resampling auf {interval_ms}ms...")

    # ZWINGEND: Sortiere die Spalten exakt so, wie das C++ Struct sie im Speicher erwartet!
    df = df[['transaction_time', 'best_bid_price', 'best_bid_qty', 'best_ask_price', 'best_ask_qty']]

    df['transaction_time'] = (df['transaction_time'] // interval_ms) * interval_ms
    df = df.drop_duplicates(subset=['transaction_time'], keep='last')
    
    compressed_rows = len(df)
    
    df = df.astype({
        'transaction_time': 'int64',
        'best_bid_price': 'float64',
        'best_bid_qty': 'float64',
        'best_ask_price': 'float64',
        'best_ask_qty': 'float64'
    })
    
    records = df.to_records(index=False)
    records.tofile(output_bin)
    
    duration = time.time() - start_time
    compression_rate = (1 - (compressed_rows / initial_rows)) * 100
    
    print("\n--- KONVERTIERUNG ABGESCHLOSSEN ---")
    print(f"Dauer:         {duration:.2f} Sekunden")
    print(f"Ticks neu:     {compressed_rows:,} (Komprimiert um {compression_rate:.1f}%)")
    print(f"Dateigröße:    {(compressed_rows * 40) / (1024*1024):.2f} MB")
    print(f"Output:        {output_bin}")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Nutzung: python l2_converter.py <input.csv>")
    else:
        convert_l2_to_binary(sys.argv[1], interval_ms=500)