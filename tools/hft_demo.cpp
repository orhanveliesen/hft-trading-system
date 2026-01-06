/**
 * HFT Demo Application
 *
 * Shared memory config'i kullanarak basit bir trading loop
 * Gerçek uygulamanın nasıl görüneceğini gösterir
 *
 * Kullanım:
 *   ./hft_demo                         # Default config
 *   ./hft_demo --config /hft_test      # Custom config
 */

#include "../include/config/shared_config.hpp"
#include "../include/strategy/simple_mean_reversion.hpp"
#include <iostream>
#include <thread>
#include <chrono>
#include <random>
#include <csignal>
#include <ctime>

using namespace hft;
using namespace hft::config;
using namespace hft::strategy;

// Global shutdown flag
std::atomic<bool> g_shutdown{false};

void signal_handler(int) {
    g_shutdown.store(true);
}

std::string timestamp() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&time));

    char result[64];
    std::snprintf(result, sizeof(result), "%s.%03ld", buf, ms.count());
    return result;
}

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [--config <name>]\n\n"
              << "Options:\n"
              << "  --config <name>    Shared memory name (default: /hft_config)\n"
              << "\nExamples:\n"
              << "  " << prog << "\n"
              << "  " << prog << " --config /hft_test\n";
}

int main(int argc, char* argv[]) {
    // Parse --config option
    const char* shm_name = SharedConfigManager::DEFAULT_SHM_NAME;

    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--config" && i + 1 < argc) {
            shm_name = argv[++i];
        } else if (std::string(argv[i]) == "--help" || std::string(argv[i]) == "-h") {
            print_usage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown option: " << argv[i] << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    // Signal handling
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Startup banner
    std::cout << "═══════════════════════════════════════════════════════════\n";
    std::cout << "  HFT Demo Application\n";
    std::cout << "═══════════════════════════════════════════════════════════\n";
    std::cout << "[" << timestamp() << "] Starting up...\n";
    std::cout << "[" << timestamp() << "] PID: " << getpid() << "\n";

    // Shared config oluştur
    std::cout << "[" << timestamp() << "] Creating shared config: /dev/shm" << shm_name << "\n";

    SharedConfig* config_ptr = nullptr;
    try {
        config_ptr = SharedConfigManager::create(shm_name);
    } catch (const std::exception& e) {
        std::cerr << "[" << timestamp() << "] ERROR: Failed to create shared config: " << e.what() << "\n";
        return 1;
    }

    if (!config_ptr) {
        std::cerr << "[" << timestamp() << "] ERROR: Failed to create shared config\n";
        return 1;
    }

    std::cout << "[" << timestamp() << "] Shared config created successfully\n";
    std::cout << "[" << timestamp() << "] Config size: " << sizeof(SharedConfig) << " bytes\n";
    std::cout << "[" << timestamp() << "] Use 'hft_control --config " << shm_name << " <command>' to control\n";
    std::cout << "───────────────────────────────────────────────────────────\n";
    std::cout << "[" << timestamp() << "] Trading started. Press Ctrl+C to exit.\n\n";

    // Strategy
    SimpleMRConfig mr_config;
    SimpleMeanReversion strategy(mr_config);

    // Fake market data
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> price_move(-5, 5);

    Price base_price = 10000;  // $100.00
    int64_t position = 0;
    int64_t pnl = 0;
    uint64_t last_sequence = 0;

    // Cleanup on exit
    auto cleanup = [&]() {
        std::cout << "\n\n[" << timestamp() << "] Shutting down...\n";
        std::cout << "[" << timestamp() << "] Final Position: " << position << "\n";
        std::cout << "[" << timestamp() << "] Final PnL: $" << (pnl + position * base_price) / 100.0 << "\n";
        std::cout << "[" << timestamp() << "] Destroying shared config: /dev/shm" << shm_name << "\n";
        SharedConfigManager::close(config_ptr);
        SharedConfigManager::destroy(shm_name);
        std::cout << "[" << timestamp() << "] Goodbye.\n";
    };

    // Main loop
    while (!g_shutdown.load()) {
        // Check kill switch FIRST
        if (config_ptr->kill_switch.load()) {
            std::cout << "\r[" << timestamp() << "] ⚠️  KILL SWITCH ACTIVE - All trading halted     ";
            std::cout.flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        // Check if config changed
        uint64_t current_seq = config_ptr->sequence.load();
        if (current_seq != last_sequence) {
            std::cout << "\n[" << timestamp() << "] Config updated (seq=" << current_seq << ")\n";
            last_sequence = current_seq;

            // Update strategy config from shared memory
            mr_config.max_position = config_ptr->max_position.load();
            mr_config.order_size = config_ptr->order_size.load();
        }

        // Generate fake market data
        base_price += price_move(gen);
        Price bid = base_price - 5;
        Price ask = base_price + 5;

        // Run strategy (if trading enabled)
        Signal signal = Signal::None;
        if (config_ptr->trading_enabled.load()) {
            signal = strategy(bid, ask, position);

            // Execute signal
            Quantity order_size = config_ptr->order_size.load();
            int64_t max_pos = config_ptr->max_position.load();

            if (signal == Signal::Buy && position < max_pos) {
                position += order_size;
                pnl -= ask * order_size;
            } else if (signal == Signal::Sell && position > -max_pos) {
                position -= order_size;
                pnl += bid * order_size;
            }
        }

        // Mark-to-market P&L
        Price mid = (bid + ask) / 2;
        int64_t mtm_pnl = pnl + position * mid;

        // Display status
        const char* signal_str = (signal == Signal::Buy) ? "BUY " :
                                 (signal == Signal::Sell) ? "SELL" : "HOLD";
        const char* trading_str = config_ptr->trading_enabled.load() ? "" : " [DISABLED]";

        std::cout << "\r"
                  << "Mid: " << mid / 100.0 << " | "
                  << "Signal: " << signal_str << " | "
                  << "Pos: " << position << " | "
                  << "PnL: $" << mtm_pnl / 100.0
                  << trading_str
                  << "          ";
        std::cout.flush();

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    cleanup();
    return 0;
}
