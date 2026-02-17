#pragma once

/**
 * CLI utilities for HFT trading applications
 *
 * Provides command-line argument parsing and related utilities.
 */

#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace hft {
namespace util {

/**
 * Command-line arguments for the trader application.
 */
struct CLIArgs {
    bool paper_mode = false;
    bool help = false;
    bool verbose = false;
    bool unified_strategy = false;  // Use unified strategy architecture
    int cpu_affinity = -1;          // CPU core to pin to (-1 = no pinning)
    std::vector<std::string> symbols;
    int duration = 0;               // 0 = unlimited
    double capital = 100000.0;
    int max_position = 10;
};

/**
 * Print help message for the trader application.
 */
inline void print_help() {
    std::cout << R"(
HFT Trading System (Lock-Free)
==============================

Usage: trader [options]

Modes:
  (default)              Production mode - REAL orders
  --paper, -p            Paper trading mode - simulated fills

Options:
  -s, --symbols SYMS     Symbols (comma-separated, default: all USDT pairs)
  -d, --duration SECS    Duration in seconds (0 = unlimited)
  -c, --capital USD      Initial capital (default: 100000)
  -m, --max-pos N        Max position per symbol (default: 10)
  --cpu N                Pin to CPU core N (reduces latency)
  --unified              Use unified strategy architecture (IStrategy + ExecutionEngine)
  -v, --verbose          Verbose output (fills, targets, stops)
  -h, --help             Show this help

Examples:
  trader --paper                      # Paper trading, all symbols
  trader --paper -s BTCUSDT,ETHUSDT   # Paper, two symbols
  trader --paper -d 300 --cpu 2       # Paper, 5 min, pinned to CPU 2
  trader --paper --restore            # Resume previous session

Monitoring:
  Use trader_observer for real-time dashboard (separate process, lock-free IPC)

WARNING: Without --paper flag, REAL orders will be sent!
)";
}

/**
 * Split a comma-separated string into a vector of trimmed, uppercase strings.
 *
 * @param s Comma-separated string (e.g., "BTCUSDT, ETHUSDT")
 * @return Vector of trimmed, uppercase symbols
 */
inline std::vector<std::string> split_symbols(const std::string& s) {
    std::vector<std::string> result;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        // Trim and uppercase
        item.erase(0, item.find_first_not_of(" \t"));
        item.erase(item.find_last_not_of(" \t") + 1);
        std::transform(item.begin(), item.end(), item.begin(), ::toupper);
        if (!item.empty()) {
            result.push_back(item);
        }
    }
    return result;
}

/**
 * Parse command-line arguments into CLIArgs struct.
 *
 * @param argc Argument count
 * @param argv Argument values
 * @param args Output argument struct
 * @return true if parsing succeeded, false on error
 */
inline bool parse_args(int argc, char* argv[], CLIArgs& args) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--paper" || arg == "-p") {
            args.paper_mode = true;
        }
        else if (arg == "--help" || arg == "-h") {
            args.help = true;
        }
        else if (arg == "--verbose" || arg == "-v") {
            args.verbose = true;
        }
        else if ((arg == "--symbols" || arg == "-s") && i + 1 < argc) {
            args.symbols = split_symbols(argv[++i]);
        }
        else if ((arg == "--duration" || arg == "-d") && i + 1 < argc) {
            args.duration = std::stoi(argv[++i]);
        }
        else if ((arg == "--capital" || arg == "-c") && i + 1 < argc) {
            args.capital = std::stod(argv[++i]);
        }
        else if ((arg == "--max-pos" || arg == "-m") && i + 1 < argc) {
            args.max_position = std::stoi(argv[++i]);
        }
        else if (arg == "--cpu" && i + 1 < argc) {
            args.cpu_affinity = std::stoi(argv[++i]);
        }
        else if (arg == "--unified") {
            args.unified_strategy = true;
        }
        else {
            std::cerr << "Unknown option: " << arg << "\n";
            std::cerr << "Use --help for usage information.\n";
            return false;
        }
    }
    return true;
}

}  // namespace util
}  // namespace hft
