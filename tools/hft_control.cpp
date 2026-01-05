/**
 * HFT Control Tool
 *
 * Shared memory üzerinden config değiştirme aracı
 *
 * Kullanım:
 *   ./hft_control status                    # Tüm config'i göster
 *   ./hft_control list                      # Parametreleri listele
 *   ./hft_control get max_position          # Tek değer oku
 *   ./hft_control set max_position 500
 *   ./hft_control kill                      # Kill switch aç
 *   ./hft_control resume                    # Kill switch kapat
 *   ./hft_control disable                   # Trading kapat
 *   ./hft_control enable                    # Trading aç
 *   ./hft_control --config /my_config status  # Farklı config dosyası
 */

#include "../include/config/shared_config.hpp"
#include <iostream>
#include <iomanip>
#include <string>
#include <cstring>
#include <chrono>

using namespace hft::config;

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [--config <name>] <command> [args...]\n\n"
              << "Options:\n"
              << "  --config <name>        Shared memory name (default: /hft_config)\n"
              << "\nCommands:\n"
              << "  status                 Show all config values\n"
              << "  list                   List all settable parameters\n"
              << "  get <param>            Get a specific value\n"
              << "  set <param> <value>    Set a specific value\n"
              << "  kill                   Activate kill switch (stop all)\n"
              << "  resume                 Deactivate kill switch\n"
              << "  disable                Disable new trades\n"
              << "  enable                 Enable trading\n"
              << "\nParameters:\n"
              << "  max_position, order_size, max_daily_loss\n"
              << "  threshold_bps, lookback_ticks, cooldown_ms\n"
              << "\nExamples:\n"
              << "  " << prog << " status\n"
              << "  " << prog << " set max_position 500\n"
              << "  " << prog << " --config /hft_prod kill\n";
}

void print_params(const SharedConfig* config, const char* shm_name) {
    std::cout << "=== Settable Parameters ===\n";
    std::cout << "Config: /dev/shm" << shm_name << "\n\n";
    std::cout << "Parameter          Current     Description\n";
    std::cout << "─────────────────────────────────────────────────────────\n";
    std::cout << "max_position       " << std::setw(10) << config->max_position.load()
              << "  Max net position (lots)\n";
    std::cout << "order_size         " << std::setw(10) << config->order_size.load()
              << "  Order size per trade (lots)\n";
    std::cout << "max_daily_loss     " << std::setw(10) << config->max_daily_loss.load()
              << "  Max daily loss (cents, $" << config->max_daily_loss.load()/100.0 << ")\n";
    std::cout << "threshold_bps      " << std::setw(10) << config->threshold_bps.load()
              << "  Signal threshold (basis points)\n";
    std::cout << "lookback_ticks     " << std::setw(10) << config->lookback_ticks.load()
              << "  Lookback window (ticks)\n";
    std::cout << "cooldown_ms        " << std::setw(10) << config->cooldown_ms.load()
              << "  Cooldown between trades (ms)\n";
    std::cout << "\n=== Read-Only ===\n";
    std::cout << "kill_switch        " << std::setw(10) << (config->kill_switch.load() ? "true" : "false")
              << "  Use 'kill' / 'resume' commands\n";
    std::cout << "trading_enabled    " << std::setw(10) << (config->trading_enabled.load() ? "true" : "false")
              << "  Use 'enable' / 'disable' commands\n";
    std::cout << "sequence           " << std::setw(10) << config->sequence.load()
              << "  Config version (auto-incremented)\n";
}

void print_status(const SharedConfig* config, const char* shm_name) {
    std::cout << "=== HFT Config Status ===\n";
    std::cout << "Config: /dev/shm" << shm_name << "\n\n";

    // Kill switches
    std::cout << "[ Control ]\n";
    std::cout << "  kill_switch:     " << (config->kill_switch.load() ? "ACTIVE ⚠️" : "off") << "\n";
    std::cout << "  trading_enabled: " << (config->trading_enabled.load() ? "yes" : "NO ⚠️") << "\n\n";

    // Position limits
    std::cout << "[ Position Limits ]\n";
    std::cout << "  max_position:    " << config->max_position.load() << "\n";
    std::cout << "  order_size:      " << config->order_size.load() << "\n";
    std::cout << "  max_daily_loss:  " << config->max_daily_loss.load() << " ($"
              << config->max_daily_loss.load() / 100.0 << ")\n\n";

    // Strategy
    std::cout << "[ Strategy ]\n";
    std::cout << "  threshold_bps:   " << config->threshold_bps.load() << " bps\n";
    std::cout << "  lookback_ticks:  " << config->lookback_ticks.load() << "\n";
    std::cout << "  cooldown_ms:     " << config->cooldown_ms.load() << " ms\n\n";

    // Metadata
    std::cout << "[ Metadata ]\n";
    std::cout << "  sequence:        " << config->sequence.load() << "\n";
    std::cout << "  version:         " << config->version << "\n";
}

uint64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()
    ).count();
}

void bump_sequence(SharedConfig* config) {
    config->sequence.fetch_add(1);
    config->last_update_ns.store(now_ns());
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    // Parse --config option
    const char* shm_name = SharedConfigManager::DEFAULT_SHM_NAME;
    int arg_offset = 1;

    if (argc >= 3 && std::string(argv[1]) == "--config") {
        shm_name = argv[2];
        arg_offset = 3;
    }

    if (argc <= arg_offset) {
        print_usage(argv[0]);
        return 1;
    }

    std::string cmd = argv[arg_offset];

    // Shared memory'e bağlan
    SharedConfig* config = SharedConfigManager::open(shm_name);
    if (!config) {
        std::cerr << "Error: Cannot open shared config at /dev/shm" << shm_name << "\n";
        std::cerr << "Is the HFT application running with this config?\n";
        return 1;
    }

    // Commands
    if (cmd == "status") {
        print_status(config, shm_name);
    }
    else if (cmd == "list") {
        print_params(config, shm_name);
    }
    else if (cmd == "get" && argc > arg_offset + 1) {
        std::string param = argv[arg_offset + 1];
        if (param == "kill_switch") {
            std::cout << config->kill_switch.load() << "\n";
        } else if (param == "trading_enabled") {
            std::cout << config->trading_enabled.load() << "\n";
        } else if (param == "max_position") {
            std::cout << config->max_position.load() << "\n";
        } else if (param == "order_size") {
            std::cout << config->order_size.load() << "\n";
        } else if (param == "max_daily_loss") {
            std::cout << config->max_daily_loss.load() << "\n";
        } else if (param == "threshold_bps") {
            std::cout << config->threshold_bps.load() << "\n";
        } else if (param == "lookback_ticks") {
            std::cout << config->lookback_ticks.load() << "\n";
        } else if (param == "cooldown_ms") {
            std::cout << config->cooldown_ms.load() << "\n";
        } else if (param == "sequence") {
            std::cout << config->sequence.load() << "\n";
        } else {
            std::cerr << "Unknown parameter: " << param << "\n";
            return 1;
        }
    }
    else if (cmd == "set" && argc > arg_offset + 2) {
        std::string param = argv[arg_offset + 1];
        std::string value = argv[arg_offset + 2];

        if (param == "max_position") {
            config->max_position.store(std::stoll(value));
            std::cout << "max_position = " << value << "\n";
        } else if (param == "order_size") {
            config->order_size.store(std::stoul(value));
            std::cout << "order_size = " << value << "\n";
        } else if (param == "max_daily_loss") {
            config->max_daily_loss.store(std::stoll(value));
            std::cout << "max_daily_loss = " << value << "\n";
        } else if (param == "threshold_bps") {
            config->threshold_bps.store(std::stoul(value));
            std::cout << "threshold_bps = " << value << "\n";
        } else if (param == "lookback_ticks") {
            config->lookback_ticks.store(std::stoul(value));
            std::cout << "lookback_ticks = " << value << "\n";
        } else if (param == "cooldown_ms") {
            config->cooldown_ms.store(std::stoul(value));
            std::cout << "cooldown_ms = " << value << "\n";
        } else {
            std::cerr << "Unknown or read-only parameter: " << param << "\n";
            return 1;
        }
        bump_sequence(config);
    }
    else if (cmd == "kill") {
        config->kill_switch.store(true);
        bump_sequence(config);
        std::cout << "⚠️  KILL SWITCH ACTIVATED\n";
    }
    else if (cmd == "resume") {
        config->kill_switch.store(false);
        bump_sequence(config);
        std::cout << "✓ Kill switch deactivated\n";
    }
    else if (cmd == "disable") {
        config->trading_enabled.store(false);
        bump_sequence(config);
        std::cout << "⚠️  Trading disabled\n";
    }
    else if (cmd == "enable") {
        config->trading_enabled.store(true);
        bump_sequence(config);
        std::cout << "✓ Trading enabled\n";
    }
    else {
        print_usage(argv[0]);
        SharedConfigManager::close(config);
        return 1;
    }

    SharedConfigManager::close(config);
    return 0;
}
