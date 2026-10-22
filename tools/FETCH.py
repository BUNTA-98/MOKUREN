import os
import urllib.request
import urllib.error
import zipfile
import argparse
import csv
import io
from datetime import datetime, timedelta

USER_AGENT = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/128.0.0.0 Safari/537.36"

def get_date_range(start_date, end_date):
    start = datetime.strptime(start_date, "%Y-%m-%d")
    end = datetime.strptime(end_date, "%Y-%m-%d")
    return [start + timedelta(days=x) for x in range((end - start).days + 1)]

def download_file(url, local_filename):
    """Downloads a file using custom headers to avoid Cloudflare/CloudFront 403 blocks."""
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req) as response, open(local_filename, "wb") as out_file:
        while chunk := response.read(1024 * 1024):  # 1MB chunks
            out_file.write(chunk)

def download_and_merge(symbol, date_list, data_type, out_filename):
    print(f"\n--- processing {data_type} ---")
    
    with open(out_filename, 'w', newline='', encoding='utf-8') as outfile:
        writer = csv.writer(outfile)
        header_written = False
        
        for d in date_list:
            date_str = d.strftime("%Y-%m-%d")
            filename = f"{symbol}-{data_type}-{date_str}.zip"
            
            # Binance Vision USD-M Futures URL
            url = f"https://data.binance.vision/data/futures/um/daily/{data_type}/{symbol}/{filename}"
            
            print(f"downloading {date_str} ({data_type})...")
            try:
                download_file(url, filename)
            except urllib.error.HTTPError as e:
                if e.code == 404:
                    print(f"  -> skipped: 404 Not Found (no archive exists for {date_str})")
                elif e.code == 403:
                    print(f"  -> skipped: 403 Forbidden (request blocked or file forbidden)")
                else:
                    print(f"  -> skipped: HTTP {e.code} ({e.reason})")
                continue
            except Exception as e:
                print(f"  -> skipped: {e}")
                continue
                
            try:
                with zipfile.ZipFile(filename, 'r') as z:
                    csv_name = z.namelist()[0]
                    with z.open(csv_name, 'r') as infile:
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
            finally:
                if os.path.exists(filename):
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
        description="mokuren data fetcher: downloads binance FUTURES trade and orderbook data for backtesting.",
        formatter_class=argparse.RawTextHelpFormatter
    )
    
    parser.add_argument("--symbol", type=str, help="e.g. BTCUSDT")
    parser.add_argument("--start", type=str, help="YYYY-MM-DD")
    parser.add_argument("--end", type=str, help="YYYY-MM-DD")
    parser.add_argument(
        "--book-type",
        type=str,
        default="bookDepth",
        choices=["bookDepth", "bookTicker"],
        help="Use 'bookDepth' for L2 order book (recommended & currently active) or 'bookTicker' (only available <= 2024-03-30)"
    )
    args = parser.parse_args()

    if not args.symbol or not args.start or not args.end:
        symbol = input("Symbol (e.g. BTCUSDT): ").strip().upper()
        if not symbol: 
            symbol = "BTCUSDT"
        start = get_valid_date("Start Date (YYYY-MM-DD): ")
        end = get_valid_date("End Date (YYYY-MM-DD): ")
        book_type = "bookDepth"
    else:
        symbol = args.symbol.upper()
        start = args.start
        end = args.end
        book_type = args.book_type

    dates = get_date_range(start, end)
    
    # 1. Aggregated Trades (Standard for execution replay)
    download_and_merge(symbol, dates, "aggTrades", f"trades.csv")
    
    # 2. L2 Order Book Depth
    download_and_merge(symbol, dates, book_type, f"L2.csv")

    print(f"\n[OK] aggTrades und {book_type} erfolgreich heruntergeladen und zusammengeführt.")

if __name__ == "__main__":
    main()
