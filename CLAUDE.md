# HFT Trading System

Low-latency C++ trading system for HFT and crypto markets. Uses pre-allocated pools, lock-free IPC, and zero-copy UDP multicast parsing.

## Architecture

### Entry Points
- `tools/trader.cpp` - Main trading engine (paper/live trading)
- `tools/trader_dashboard.cpp` - Real-time ImGui dashboard (reads IPC)
- `tools/trader_observer.cpp` - CLI event monitor (reads IPC)
- `tools/trader_control.cpp` - Runtime parameter control (writes IPC)
- `tools/run_backtest.cpp` - Backtesting engine
- `tools/optimize_strategies.cpp` - Parameter optimizer

### Data Flow
```
UDP Multicast (ITCH 5.0)
  → UdpReceiver (epoll)
    → PacketBuffer (lock-free ring)
      → FeedHandler (template parser)
        → MarketDataHandler (adapter)
          → OrderBook (pre-allocated pools)
            → Strategy (position tracker + risk)
              → IPC (shared memory events)
                → Dashboard/Observer (readers)
```

### Core Modules
- **OrderBook** (`include/orderbook.hpp`) - Pre-allocated pools (~160MB), intrusive lists, O(1) ops
- **FeedHandler** (`include/feed_handler.hpp`) - Template-based ITCH parser, zero vtable overhead
- **PacketBuffer** (`include/network/packet_buffer.hpp`) - Lock-free SPSC ring buffer
- **IPC** (`include/ipc/*.hpp`) - Shared memory: config, portfolio state, event stream, lifecycle
- **Strategy** (`include/strategy/*.hpp`) - Position tracking, risk management, market making

### Hot Path Files (benchmark before/after changes)
- `include/orderbook.hpp`
- `include/feed_handler.hpp`
- `include/network/packet_buffer.hpp`
- `include/strategy/market_maker.hpp`

## Commands

```bash
# Build (CMake + Make)
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)

# Test
ctest --output-on-failure

# Benchmark (mandatory for hot path changes)
./bench_orderbook

# Run paper trading
./trader --paper

# Run live dashboard (separate terminal)
./trader_dashboard

# Run event monitor (separate terminal)
./trader_observer

# Control runtime params
./trader_control --set-maker-spread 0.002

# Backtest
./run_backtest --symbol BTCUSDT --start 2024-01-01 --end 2024-01-31

# Optimize strategy params
./optimize_strategies --symbol BTCUSDT --metric sharpe

# Format code (before commit)
find include tools tests benchmarks -type f \( -name "*.cpp" -o -name "*.hpp" \) -exec clang-format -i {} +
```

## CI/CD & Code Coverage

### GitHub Actions Workflows
- **build-test.yml**: Build (Release) + run all 56 tests on every push/PR
- **lint.yml**: Enforce clang-format on all .cpp/.hpp files
- **codecov.yml**: Generate coverage report with 100% threshold

### Code Coverage Requirements
- **Target**: 100% line coverage (strict)
- **Tool**: lcov + gcov
- **Exclusions**: `/usr/*`, `*/external/*`, `*/tests/*`
- **Unreachable error paths**: Mark with `LCOV_EXCL_LINE` comment
  ```cpp
  if (unlikely_error_condition) { // LCOV_EXCL_LINE
      handle_unreachable_error(); // LCOV_EXCL_LINE
  } // LCOV_EXCL_LINE
  ```

### Formatting (clang-format)
- **Style**: LLVM-based, HFT-aware (120 col limit, compact hot path)
- **Enforcement**: CI fails on formatting violations
- **Local check**: `clang-format --dry-run --Werror <file>`
- **Auto-fix**: See format command above

## Docker Build Image

### Builder Image
- **Image**: `ghcr.io/orhanveliesen/hft-builder:latest`
- **Base**: Ubuntu 22.04
- **Pre-installed**: libwebsockets, glfw, curl, cmake, build-essential, clang-format, lcov
- **Source**: `docker/Dockerfile`
- **Purpose**: Speed up CI/CD by pre-installing dependencies (20-30x faster than apt install)

### Local Usage
```bash
# Pull latest image
docker pull ghcr.io/orhanveliesen/hft-builder:latest

# Build project in container
docker run --rm -v $(pwd):/workspace ghcr.io/orhanveliesen/hft-builder:latest \
  bash -c "mkdir -p build && cd build && cmake -DCMAKE_BUILD_TYPE=Release .. && make -j$(nproc)"

# Run tests
docker run --rm -v $(pwd):/workspace ghcr.io/orhanveliesen/hft-builder:latest \
  bash -c "cd build && ctest --output-on-failure"

# Format code
docker run --rm -v $(pwd):/workspace ghcr.io/orhanveliesen/hft-builder:latest \
  bash -c "find include tools tests benchmarks -type f \( -name '*.cpp' -o -name '*.hpp' \) -exec clang-format -i {} +"
```

### Rebuilding Image
```bash
# Manually trigger rebuild via GitHub Actions
gh workflow run docker-build.yml

# Or build locally and push
docker build -t ghcr.io/orhanveliesen/hft-builder:latest -f docker/Dockerfile .
docker push ghcr.io/orhanveliesen/hft-builder:latest
```

**Note**: Docker build workflow (`.github/workflows/docker-build.yml`) automatically rebuilds and pushes the image when `docker/Dockerfile` changes.

### CI/CD Integration
All GitHub Actions workflows use the builder image:
- **build-test.yml**: Compiles and runs 53 test suites
- **lint.yml**: Enforces clang-format-16 (C++23 support)
- **codecov.yml**: Generates coverage reports with lcov

Dependencies are pre-installed in the image, reducing workflow runtime from ~60s to ~5s for dependency setup.

## Project-Specific Constraints

### Hot Path Parameter Exception
Global rule: 3+ parameters = use Input struct
**Exception**: Protocol callbacks allowed 3+ params (no struct overhead at nanosecond scale)
```cpp
// ✅ ALLOWED - ITCH callbacks
void on_add_order(OrderId, Side, Price, Quantity);
void on_trade(Symbol, Price, Quantity, Side);

// ❌ STILL FORBIDDEN - Non-hot-path
void create_report(string, string, int, double, bool);  // Use Input struct
```

### Interface Exception
Global rule: Single implementation = no interface
**Exception**: `IExchange` interface allowed (Binance adapter exists, Coinbase/Kraken planned)

### Performance Baselines (DO NOT REGRESS)
```
OrderBook:
  - Cancel: < 500 ns
  - Execute: < 500 ns
  - Best Bid/Ask: < 25 ns
  - Throughput: > 2M ops/sec

IPC (relaxed memory order):
  - Price update: < 5 cycles
  - Position update: < 10 cycles
  - Heartbeat: < 100 cycles
```

Hot path changes require benchmark before/after. Any regression = immediate revert.

### IPC Testing Requirement
Any IPC struct change must be tested with all consumers running:
```bash
# Terminal 1: ./trader --paper
# Terminal 2: ./trader_dashboard
# Terminal 3: ./trader_observer
# All three must work without errors
```

Changing memory layout breaks backward compatibility - requires explicit approval.

### Trader Lifecycle
- **Heartbeat**: Updated every 1s, dashboard detects stale (>3s) and shows warning
- **Version check**: Git commit hash embedded at compile-time, auto-invalidates mismatched shared memory
- **Graceful shutdown**: Signal handlers (SIGTERM/SIGINT) set status before exit

### Hot Path Constraints
No allocations, no virtual calls, no syscalls on hot path. Use C++20 concepts for polymorphism (not virtual functions). Logging: use shared memory events (TunerEvent, TradeEvent), not stdout.

### Memory Order (IPC)
- Single-writer: `std::memory_order_relaxed`
- Cross-thread sync: `acquire/release`
- When in doubt: ASK before implementing

## Project Structure
```
include/
  orderbook.hpp, feed_handler.hpp, market_data_handler.hpp
  network/udp_receiver.hpp, packet_buffer.hpp
  ipc/shared_config.hpp, shared_portfolio_state.hpp, shared_ring_buffer.hpp, trade_event.hpp
  strategy/position.hpp, risk_manager.hpp, market_maker.hpp, smart_strategy.hpp
  benchmark/timer.hpp, histogram.hpp
tools/
  trader.cpp, trader_dashboard.cpp, trader_observer.cpp, trader_control.cpp
  run_backtest.cpp, optimize_strategies.cpp
tests/ (33 test suites)
benchmarks/bench_orderbook.cpp
```

## References
- NASDAQ ITCH 5.0: https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHspecification.pdf
- C++ Core Guidelines: https://isocpp.github.io/CppCoreGuidelines/
