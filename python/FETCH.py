import os
import urllib.request
import zipfile
import argparse
import csv
from datetime import datetime, timedelta

def get_date_range(start_date, end_date):
    start = datetime.strptime(start_date, "%Y-%m-%d")
    end = datetime.strptime(end_date, "%Y-%m-%d")
    return [start + timedelta(days=x) for x in range((end - start).days + 1)]

def download_and_merge(symbol, date_list, data_type, out_filename):
    print(f"\n--- processing {data_type} ---")
    
    with open(out_filename, 'w', newline='', encoding='utf-8') as outfile:
        writer = csv.writer(outfile)
        header_written = False
        
        for d in date_list:
            date_str = d.strftime("%Y-%m-%d")
            filename = f"{symbol}-{data_type}-{date_str}.zip"
            
            # WICHTIG: Zurück auf Futures
            url = f"https://data.binance.vision/data/futures/um/daily/{data_type}/{symbol}/{filename}"
            
            print(f"downloading {date_str}...")
            try:
                urllib.request.urlretrieve(url, filename)
            except Exception as e:
                print(f"  -> skipped (no data found for {date_str})")
                continue
                
            with zipfile.ZipFile(filename, 'r') as z:
                csv_name = z.namelist()[0]
                with z.open(csv_name, 'r') as infile:
                    import io
                    text_stream = io.TextIOWrapper(infile, encoding='utf-8')
                    reader = csv.reader(text_stream)
                    
                    is_first_line = True
                    for row in reader:
                        if is_first_line:
                            is_first_line = False
                            if not header_written:
                                writer.writerow(row)
                                header_written = True
                        else:
                            writer.writerow(row)
            
            os.remove(filename)
            print(f"  -> {date_str} merged.")

def get_valid_date(prompt):
    while True:
        val = input(prompt).strip()
        try:
            datetime.strptime(val, "%Y-%m-%d")
            return val
        except ValueError:
            print("  [!] error: use YYYY-MM-DD format (e.g. 2026-09-01).")

def main():
    parser = argparse.ArgumentParser(
        description="mokuren data fetcher: downloads binance FUTURES trade data for backtesting.",
        formatter_class=argparse.RawTextHelpFormatter
    )
    
    parser.add_argument("--symbol", type=str, help="e.g. BTCUSDT")
    parser.add_argument("--start", type=str, help="YYYY-MM-DD")
    parser.add_argument("--end", type=str, help="YYYY-MM-DD")
    args = parser.parse_args()

    if not args.symbol or not args.start or not args.end:
        symbol = input("Symbol (e.g. BTCUSDT): ").strip().upper()
        if not symbol: symbol = "BTCUSDT"
        start = get_valid_date("Start Date (YYYY-MM-DD): ")
        end = get_valid_date("End Date (YYYY-MM-DD): ")
    else:
        symbol = args.symbol
        start = args.start
        end = args.end

    dates = get_date_range(start, end)
    
    # Wir laden nur die Trades, da Binance kein tiefes Orderbuch kostenlos liefert
    download_and_merge(symbol, dates, "trades", "trades.csv")

    print("\n[OK] Trades erfolgreich heruntergeladen und zusammengefuehrt.")

if __name__ == "__main__":
    main()