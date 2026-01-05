#!/usr/bin/env python3
"""
Binance Historical Data Downloader
Downloads klines (OHLCV) and aggTrades data from Binance public data repository.
Data source: https://data.binance.vision/
"""

import os
import sys
import requests
import zipfile
import io
from datetime import datetime, timedelta
from pathlib import Path
import argparse

BASE_URL = "https://data.binance.vision/data/spot"

def download_file(url: str, output_path: Path) -> bool:
    """Download a file from URL to output path."""
    try:
        response = requests.get(url, stream=True, timeout=30)
        if response.status_code == 200:
            with open(output_path, 'wb') as f:
                for chunk in response.iter_content(chunk_size=8192):
                    f.write(chunk)
            return True
        return False
    except Exception as e:
        print(f"Error downloading {url}: {e}")
        return False

def download_and_extract_zip(url: str, output_dir: Path) -> bool:
    """Download and extract a zip file."""
    try:
        response = requests.get(url, timeout=60)
        if response.status_code != 200:
            return False

        with zipfile.ZipFile(io.BytesIO(response.content)) as z:
            z.extractall(output_dir)
        return True
    except Exception as e:
        print(f"Error: {e}")
        return False

def download_klines(symbol: str, interval: str, start_date: datetime,
                    end_date: datetime, output_dir: Path) -> list:
    """
    Download kline (candlestick) data.

    Intervals: 1s, 1m, 3m, 5m, 15m, 30m, 1h, 2h, 4h, 6h, 8h, 12h, 1d, 3d, 1w, 1mo
    """
    output_dir.mkdir(parents=True, exist_ok=True)
    downloaded_files = []

    current = start_date
    while current <= end_date:
        year_month = current.strftime("%Y-%m")
        filename = f"{symbol}-{interval}-{year_month}.zip"
        url = f"{BASE_URL}/monthly/klines/{symbol}/{interval}/{filename}"

        print(f"Downloading {filename}...", end=" ")

        if download_and_extract_zip(url, output_dir):
            csv_file = output_dir / f"{symbol}-{interval}-{year_month}.csv"
            if csv_file.exists():
                downloaded_files.append(csv_file)
                print("OK")
            else:
                print("Extracted but CSV not found")
        else:
            print("Not available")

        # Move to next month
        if current.month == 12:
            current = current.replace(year=current.year + 1, month=1)
        else:
            current = current.replace(month=current.month + 1)

    return downloaded_files

def download_aggtrades(symbol: str, start_date: datetime,
                       end_date: datetime, output_dir: Path) -> list:
    """
    Download aggregated trades data (tick-level).
    This is the most granular data available.
    """
    output_dir.mkdir(parents=True, exist_ok=True)
    downloaded_files = []

    current = start_date
    while current <= end_date:
        year_month = current.strftime("%Y-%m")
        filename = f"{symbol}-aggTrades-{year_month}.zip"
        url = f"{BASE_URL}/monthly/aggTrades/{symbol}/{filename}"

        print(f"Downloading {filename}...", end=" ")

        if download_and_extract_zip(url, output_dir):
            csv_file = output_dir / f"{symbol}-aggTrades-{year_month}.csv"
            if csv_file.exists():
                downloaded_files.append(csv_file)
                print("OK")
            else:
                print("Extracted but CSV not found")
        else:
            print("Not available")

        # Move to next month
        if current.month == 12:
            current = current.replace(year=current.year + 1, month=1)
        else:
            current = current.replace(month=current.month + 1)

    return downloaded_files

def merge_csv_files(files: list, output_file: Path, data_type: str):
    """Merge multiple CSV files into one."""
    print(f"\nMerging {len(files)} files into {output_file}...")

    with open(output_file, 'w') as outfile:
        # Write header based on data type
        if data_type == "klines":
            outfile.write("open_time,open,high,low,close,volume,close_time,"
                         "quote_volume,trades,taker_buy_base,taker_buy_quote,ignore\n")
        elif data_type == "aggtrades":
            outfile.write("agg_trade_id,price,quantity,first_trade_id,"
                         "last_trade_id,timestamp,is_buyer_maker,is_best_match\n")

        for f in sorted(files):
            with open(f, 'r') as infile:
                for line in infile:
                    outfile.write(line)

    print(f"Created {output_file} ({output_file.stat().st_size / 1024 / 1024:.1f} MB)")

def convert_to_backtest_format(input_file: Path, output_file: Path, data_type: str):
    """
    Convert Binance data to our backtester format.
    Output: timestamp,bid,ask,bid_size,ask_size

    For klines: Use close price as mid, simulate spread
    For aggTrades: Use actual trade prices
    """
    print(f"\nConverting to backtest format...")

    with open(input_file, 'r') as infile, open(output_file, 'w') as outfile:
        outfile.write("timestamp,bid,ask,bid_size,ask_size\n")

        # Skip header if present
        first_line = infile.readline()
        if not first_line[0].isdigit():
            pass  # It was a header, continue
        else:
            infile.seek(0)  # Not a header, go back

        for line in infile:
            parts = line.strip().split(',')

            if data_type == "klines":
                # Kline format: open_time,open,high,low,close,volume,...
                timestamp = int(parts[0])

                # Normalize timestamp to milliseconds
                # Milliseconds: 13 digits (1700000000000 = Nov 2023)
                # Microseconds: 16 digits (1700000000000000)
                if len(str(timestamp)) > 13:  # More than 13 digits = microseconds
                    timestamp = timestamp // 1000

                close = float(parts[4])
                volume = float(parts[5])

                # Convert to fixed-point (4 decimals)
                # Binance prices are in quote currency (e.g., USDT)
                price_int = int(close * 10000)

                # Simulate spread based on volatility (high-low)
                high = float(parts[2])
                low = float(parts[3])
                spread = max(1, int((high - low) * 10000 * 0.1))  # 10% of range
                half_spread = spread // 2

                bid = price_int - half_spread
                ask = price_int + half_spread
                size = int(volume)

                outfile.write(f"{timestamp},{bid},{ask},{size},{size}\n")

            elif data_type == "aggtrades":
                # AggTrade format: agg_trade_id,price,quantity,first_id,last_id,timestamp,is_buyer,is_best
                timestamp = int(parts[5])
                price = float(parts[1])
                quantity = float(parts[2])
                is_buyer = parts[6].lower() == 'true'

                price_int = int(price * 10000)
                size = int(quantity)

                # Simulate BBO from trade direction
                # If buyer maker (seller aggressor), price is at bid
                # If seller maker (buyer aggressor), price is at ask
                spread = max(1, price_int // 10000)  # ~1 bps spread

                if is_buyer:
                    bid = price_int
                    ask = price_int + spread
                else:
                    bid = price_int - spread
                    ask = price_int

                outfile.write(f"{timestamp},{bid},{ask},{size},{size}\n")

    print(f"Created {output_file}")

def main():
    parser = argparse.ArgumentParser(description="Download Binance historical data")
    parser.add_argument("--symbol", default="BTCUSDT", help="Trading pair (default: BTCUSDT)")
    parser.add_argument("--type", choices=["klines", "aggtrades"], default="klines",
                       help="Data type (default: klines)")
    parser.add_argument("--interval", default="1m", help="Kline interval (default: 1m)")
    parser.add_argument("--years", type=int, default=2, help="Years of data (default: 2)")
    parser.add_argument("--output-dir", default="data/binance", help="Output directory")

    args = parser.parse_args()

    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    end_date = datetime.now()
    start_date = end_date - timedelta(days=args.years * 365)

    print(f"=== Binance Data Downloader ===")
    print(f"Symbol: {args.symbol}")
    print(f"Type: {args.type}")
    print(f"Period: {start_date.date()} to {end_date.date()}")
    print(f"Output: {output_dir}\n")

    if args.type == "klines":
        files = download_klines(args.symbol, args.interval, start_date, end_date,
                               output_dir / "raw")
        merged_file = output_dir / f"{args.symbol}-{args.interval}-merged.csv"
    else:
        files = download_aggtrades(args.symbol, start_date, end_date, output_dir / "raw")
        merged_file = output_dir / f"{args.symbol}-aggTrades-merged.csv"

    if files:
        merge_csv_files(files, merged_file, args.type)

        backtest_file = output_dir / f"{args.symbol}-backtest.csv"
        convert_to_backtest_format(merged_file, backtest_file, args.type)

        print(f"\n=== Done ===")
        print(f"Raw files: {output_dir}/raw/")
        print(f"Merged: {merged_file}")
        print(f"Backtest format: {backtest_file}")
    else:
        print("\nNo data downloaded!")

if __name__ == "__main__":
    main()
