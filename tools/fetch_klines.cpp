/**
 * Binance Kline Fetcher
 *
 * Downloads historical candlestick data from Binance and saves to CSV.
 *
 * Usage:
 *   ./fetch_klines BTCUSDT 1h 2024-01-01 2024-12-31 output.csv
 *   ./fetch_klines ETHUSDT 5m 2024-06-01 2024-06-30
 */

#include "../include/exchange/binance_rest.hpp"
#include "../include/exchange/market_data.hpp"

#include <cstring>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <unistd.h>

using namespace hft;
using namespace hft::exchange;

void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " SYMBOL INTERVAL START_DATE [END_DATE] [OUTPUT_FILE]\n"
              << "\n"
              << "Arguments:\n"
              << "  SYMBOL      Trading pair (e.g., BTCUSDT, ETHUSDT)\n"
              << "  INTERVAL    Kline interval: 1m, 5m, 15m, 1h, 4h, 1d\n"
              << "  START_DATE  Start date (YYYY-MM-DD)\n"
              << "  END_DATE    End date (YYYY-MM-DD), default: today\n"
              << "  OUTPUT_FILE Output CSV file, default: SYMBOL_INTERVAL.csv\n"
              << "\n"
              << "Examples:\n"
              << "  " << prog << " BTCUSDT 1h 2024-01-01 2024-12-31 btc_hourly.csv\n"
              << "  " << prog << " ETHUSDT 5m 2024-06-01\n";
}

// Parse date string to timestamp (milliseconds)
Timestamp parse_date(const std::string& date_str) {
    struct tm tm = {};
    if (strptime(date_str.c_str(), "%Y-%m-%d", &tm) == nullptr) {
        throw std::runtime_error("Invalid date format: " + date_str);
    }
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;

    time_t t = timegm(&tm); // UTC
    return static_cast<Timestamp>(t) * 1000;
}

// Format timestamp to readable string
std::string format_timestamp(Timestamp ts) {
    time_t t = ts / 1000;
    struct tm tm;
    gmtime_r(&t, &tm);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

int main(int argc, char* argv[]) {
    if (argc < 4) {
        print_usage(argv[0]);
        return 1;
    }

    std::string symbol = argv[1];
    std::string interval = argv[2];
    std::string start_date = argv[3];
    std::string end_date;
    std::string output_file;

    // Parse optional arguments
    if (argc >= 5) {
        end_date = argv[4];
    } else {
        // Default: today
        time_t now = time(nullptr);
        struct tm tm;
        gmtime_r(&now, &tm);
        char buf[16];
        strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
        end_date = buf;
    }

    if (argc >= 6) {
        output_file = argv[5];
    } else {
        output_file = symbol + "_" + interval + ".csv";
    }

    // Validate interval
    const char* valid_intervals[] = {"1m", "5m", "15m", "1h", "4h", "1d"};
    bool valid = false;
    for (const char* v : valid_intervals) {
        if (interval == v) {
            valid = true;
            break;
        }
    }
    if (!valid) {
        std::cerr << "Error: Invalid interval '" << interval << "'\n";
        std::cerr << "Valid intervals: 1m, 5m, 15m, 1h, 4h, 1d\n";
        return 1;
    }

    try {
        // Parse dates
        Timestamp start_ts = parse_date(start_date);
        Timestamp end_ts = parse_date(end_date);
        end_ts += 24 * 60 * 60 * 1000 - 1; // End of day

        std::cout << "Fetching " << symbol << " " << interval << " klines\n";
        std::cout << "From: " << format_timestamp(start_ts) << " UTC\n";
        std::cout << "To:   " << format_timestamp(end_ts) << " UTC\n";
        std::cout << "\n";

        // Create client
        BinanceRest client(false); // Use mainnet

        // Test connection
        std::cout << "Checking server time... ";
        Timestamp server_time = client.get_server_time();
        std::cout << format_timestamp(server_time) << " UTC\n";

        // Check current price
        std::cout << "Current " << symbol << " price: $";
        double price = client.get_price(symbol);
        std::cout << std::fixed << std::setprecision(2) << price << "\n\n";

        // Fetch klines
        std::cout << "Downloading klines";
        std::cout.flush();

        std::vector<Kline> klines;
        Timestamp current_start = start_ts;
        const int limit = 1000;
        int batch_count = 0;

        while (current_start < end_ts) {
            auto batch = client.fetch_klines(symbol, interval, current_start, end_ts, limit);

            if (batch.empty()) {
                break;
            }

            klines.insert(klines.end(), batch.begin(), batch.end());
            current_start = batch.back().close_time + 1;

            batch_count++;
            if (batch_count % 10 == 0) {
                std::cout << ".";
                std::cout.flush();
            }

            // Rate limiting
            usleep(100000); // 100ms
        }

        std::cout << "\n\n";
        std::cout << "Downloaded " << klines.size() << " klines in " << batch_count << " requests\n";

        if (klines.empty()) {
            std::cerr << "No data found for the specified range.\n";
            return 1;
        }

        // Print summary
        std::cout << "\nData Summary:\n";
        std::cout << "  First: " << format_timestamp(klines.front().open_time) << "\n";
        std::cout << "  Last:  " << format_timestamp(klines.back().open_time) << "\n";

        Price min_price = klines[0].low;
        Price max_price = klines[0].high;
        double total_volume = 0;

        for (const auto& k : klines) {
            if (k.low < min_price)
                min_price = k.low;
            if (k.high > max_price)
                max_price = k.high;
            total_volume += k.volume;
        }

        std::cout << "  Low:   $" << std::fixed << std::setprecision(2) << (min_price / 10000.0) << "\n";
        std::cout << "  High:  $" << (max_price / 10000.0) << "\n";
        std::cout << "  Volume: " << std::setprecision(0) << total_volume << " " << symbol.substr(0, 3) << "\n";

        // Save to CSV
        std::cout << "\nSaving to " << output_file << "... ";
        save_klines_csv(output_file, klines);
        std::cout << "done!\n";

        return 0;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
