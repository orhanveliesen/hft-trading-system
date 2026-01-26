/**
 * HFT Telemetry Collector
 *
 * Receives UDP multicast telemetry from HFT engine and displays it.
 * In production, this would forward to a time-series database (QuestDB, InfluxDB).
 *
 * Usage:
 *   hft_telemetry_collector                    # Default multicast 239.255.0.1:5555
 *   hft_telemetry_collector -a 239.255.0.2 -p 5556  # Custom address
 */

#include "../include/ipc/udp_telemetry.hpp"
#include <iostream>
#include <iomanip>
#include <csignal>
#include <atomic>
#include <chrono>
#include <cstring>

using namespace hft::ipc;

std::atomic<bool> g_running{true};

void signal_handler(int) {
    g_running = false;
}

const char* type_name(TelemetryType t) {
    switch (t) {
        case TelemetryType::Heartbeat: return "HEARTBEAT";
        case TelemetryType::Quote:     return "QUOTE";
        case TelemetryType::Fill:      return "FILL";
        case TelemetryType::Order:     return "ORDER";
        case TelemetryType::Position:  return "POSITION";
        case TelemetryType::PnL:       return "PNL";
        case TelemetryType::Regime:    return "REGIME";
        case TelemetryType::Risk:      return "RISK";
        case TelemetryType::Latency:   return "LATENCY";
        default:                       return "UNKNOWN";
    }
}

void print_packet(const TelemetryPacket& pkt) {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto tm = std::localtime(&time);

    std::cout << std::setfill('0')
              << std::setw(2) << tm->tm_hour << ":"
              << std::setw(2) << tm->tm_min << ":"
              << std::setw(2) << tm->tm_sec << " ";

    std::cout << "[" << std::setw(5) << pkt.sequence << "] "
              << std::setw(9) << type_name(pkt.type);

    if (pkt.symbol_id > 0) {
        std::cout << " sym=" << pkt.symbol_id;
    }

    std::cout << std::setfill(' ');

    switch (pkt.type) {
        case TelemetryType::Heartbeat:
            std::cout << " (alive)";
            break;

        case TelemetryType::Quote:
            std::cout << std::fixed << std::setprecision(2)
                      << " bid=" << (pkt.data.quote.bid_price / 1e8)
                      << " ask=" << (pkt.data.quote.ask_price / 1e8)
                      << " spread=" << ((pkt.data.quote.ask_price - pkt.data.quote.bid_price) / 1e8);
            break;

        case TelemetryType::Fill:
            std::cout << " " << (pkt.data.fill.side == 0 ? "BUY" : "SELL")
                      << std::fixed << std::setprecision(2)
                      << " qty=" << pkt.data.fill.quantity
                      << " price=$" << (pkt.data.fill.price / 1e8);
            break;

        case TelemetryType::Position:
            std::cout << std::fixed << std::setprecision(4)
                      << " qty=" << (pkt.data.position.quantity / 1e8)
                      << " avg=$" << (pkt.data.position.avg_price / 1e8)
                      << " unrealized=$" << std::setprecision(2)
                      << (pkt.data.position.unrealized_pnl / 1e8);
            break;

        case TelemetryType::PnL:
            std::cout << std::fixed << std::setprecision(2)
                      << " realized=$" << (pkt.data.pnl.realized_pnl / 1e8)
                      << " unrealized=$" << (pkt.data.pnl.unrealized_pnl / 1e8)
                      << " equity=$" << (pkt.data.pnl.total_equity / 1e8)
                      << " wins=" << pkt.data.pnl.win_count
                      << " losses=" << pkt.data.pnl.loss_count;
            break;

        case TelemetryType::Regime:
            std::cout << " regime=" << (int)pkt.data.regime.regime
                      << " confidence=" << (int)pkt.data.regime.confidence << "%";
            break;

        case TelemetryType::Latency:
            std::cout << " tick→decision=" << pkt.data.latency.tick_to_decision_ns << "ns"
                      << " decision→order=" << pkt.data.latency.decision_to_order_ns << "ns"
                      << " total=" << pkt.data.latency.total_roundtrip_ns << "ns";
            break;

        default:
            break;
    }

    std::cout << "\n";
}

void print_help() {
    std::cout << R"(
HFT Telemetry Collector
=======================

Receives UDP multicast telemetry from HFT engine.

Usage: hft_telemetry_collector [options]

Options:
  -a, --address ADDR   Multicast address (default: 239.255.0.1)
  -p, --port PORT      UDP port (default: 5555)
  -q, --quiet          Only show fills and P&L updates
  -h, --help           Show this help

Examples:
  hft_telemetry_collector                # Default settings
  hft_telemetry_collector -q             # Quiet mode (fills/PnL only)

In production, forward to time-series DB:
  hft_telemetry_collector | influx write ...
)";
}

int main(int argc, char* argv[]) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    const char* address = "239.255.0.1";
    uint16_t port = 5555;
    bool quiet = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_help();
            return 0;
        } else if ((arg == "-a" || arg == "--address") && i + 1 < argc) {
            address = argv[++i];
        } else if ((arg == "-p" || arg == "--port") && i + 1 < argc) {
            port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "-q" || arg == "--quiet") {
            quiet = true;
        }
    }

    std::cout << "HFT Telemetry Collector\n";
    std::cout << "=======================\n";
    std::cout << "Listening on " << address << ":" << port << "\n";
    if (quiet) std::cout << "Quiet mode: showing fills and P&L only\n";
    std::cout << "Press Ctrl+C to exit\n\n";

    TelemetrySubscriber sub(address, port);
    if (!sub.is_valid()) {
        std::cerr << "ERROR: Failed to create subscriber\n";
        return 1;
    }

    sub.set_callback([quiet](const TelemetryPacket& pkt) {
        if (quiet) {
            // Only show fills and P&L in quiet mode
            if (pkt.type != TelemetryType::Fill &&
                pkt.type != TelemetryType::PnL) {
                return;
            }
        }
        print_packet(pkt);
    });

    sub.start();

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    sub.stop();

    std::cout << "\n--- Statistics ---\n";
    std::cout << "Packets received: " << sub.packets_received() << "\n";
    std::cout << "Packets dropped:  " << sub.packets_dropped() << "\n";

    if (sub.packets_dropped() > 0) {
        double loss_rate = 100.0 * sub.packets_dropped() /
            (sub.packets_received() + sub.packets_dropped());
        std::cout << "Loss rate:        " << std::fixed << std::setprecision(2)
                  << loss_rate << "%\n";
    }

    return 0;
}
