# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

HFT training project for learning low-latency trading systems in C++ and Rust. Target: US HFT firm applications.

## Build Commands

```bash
# CMake build (recommended)
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)

# Run all tests
ctest --output-on-failure

# Run benchmark
./bench_orderbook

# Rust build
cd rust && cargo build --release
cargo test
./target/release/bench_orderbook
```

## Architecture

```
                    ┌─────────────────┐
                    │  UDP Multicast  │
                    │   (epoll)       │
                    └────────┬────────┘
                             │ raw packets
                    ┌────────▼────────┐
                    │  PacketBuffer   │
                    │  (ring buffer)  │
                    └────────┬────────┘
                             │ MoldUDP64
                    ┌────────▼────────┐
                    │  FeedHandler    │
                    │  (ITCH parser)  │
                    └────────┬────────┘
                             │ callbacks
                    ┌────────▼────────┐
                    │ MarketDataHandler│
                    │   (adapter)     │
                    └────────┬────────┘
                             │
                    ┌────────▼────────┐
                    │   OrderBook     │
                    │ (pre-allocated) │
                    └─────────────────┘
```

### Core Components
- **OrderBook** (`include/orderbook.hpp`) - Pre-allocated pools (~160MB), intrusive linked list, O(1) add/cancel/execute
- **FeedHandler** (`include/feed_handler.hpp`) - Template-based ITCH 5.0 parser, no vtable overhead
- **MarketDataHandler** (`include/market_data_handler.hpp`) - Feed→OrderBook adapter
- **UdpReceiver** (`include/network/udp_receiver.hpp`) - epoll-based multicast receiver
- **PacketBuffer** (`include/network/packet_buffer.hpp`) - Lock-free SPSC ring buffer
- **Strategy** (`include/strategy/`) - Position tracker, risk manager, market maker
- **Benchmark** (`include/benchmark/`) - RDTSC timer, histogram

### IPC Components (Shared Memory)
- **SharedConfig** (`include/ipc/shared_config.hpp`) - Runtime config, heartbeat, lifecycle management
- **SharedPortfolioState** (`include/ipc/shared_portfolio_state.hpp`) - Portfolio snapshot for dashboard
- **SharedRingBuffer** (`include/ipc/shared_ring_buffer.hpp`) - Lock-free SPSC event stream
- **TradeEvent** (`include/ipc/trade_event.hpp`) - Event types for observer/dashboard

### Tools
- **hft** (`tools/hft.cpp`) - Main trading engine (paper trading on Binance)
- **hft_dashboard** (`tools/hft_dashboard.cpp`) - Real-time ImGui dashboard
- **hft_observer** (`tools/hft_observer.cpp`) - Event stream monitor (CLI)
- **hft_control** (`tools/hft_control.cpp`) - Runtime parameter control
- **run_backtest** (`tools/run_backtest.cpp`) - Strategy backtesting
- **optimize_strategies** (`tools/optimize_strategies.cpp`) - Parameter optimization

## Code Standards (HFT-Specific)

### Hot Path Rules
```cpp
// FORBIDDEN on hot path
new, delete, malloc, free
std::string, std::map, std::unordered_map
virtual functions, exceptions

// ALLOWED on hot path
Pre-allocated arrays, intrusive containers
Direct array indexing, inline functions, constexpr
```

### Naming Convention
```cpp
class OrderBook {           // PascalCase for classes
    uint32_t best_bid_;     // snake_case_ trailing underscore for members
    void add_order();       // snake_case for methods
};
constexpr size_t MAX_ORDERS = 1'000'000;  // UPPER_SNAKE for constants
```

## Project Structure
```
hft/
├── include/
│   ├── types.hpp              # Core types (Order, Price, Side)
│   ├── orderbook.hpp          # Order book interface
│   ├── itch_messages.hpp      # ITCH 5.0 message definitions
│   ├── feed_handler.hpp       # Binary protocol parser
│   ├── market_data_handler.hpp # Feed→OrderBook adapter
│   ├── network/
│   │   ├── udp_receiver.hpp   # Multicast receiver + epoll
│   │   └── packet_buffer.hpp  # Lock-free ring buffer
│   ├── ipc/
│   │   ├── shared_config.hpp  # Runtime config + heartbeat
│   │   ├── shared_portfolio_state.hpp # Portfolio snapshot
│   │   ├── shared_ring_buffer.hpp # Lock-free event stream
│   │   └── trade_event.hpp    # Event definitions
│   ├── benchmark/
│   │   ├── timer.hpp          # RDTSC timing
│   │   └── histogram.hpp      # Latency histogram
│   └── strategy/
│       ├── position.hpp       # Position & P&L tracking
│       ├── risk_manager.hpp   # Risk limits
│       └── market_maker.hpp   # Market making strategy
├── tools/
│   ├── hft.cpp                # Main trading engine
│   ├── hft_dashboard.cpp      # ImGui real-time dashboard
│   ├── hft_observer.cpp       # CLI event monitor
│   ├── hft_control.cpp        # Runtime parameter control
│   ├── run_backtest.cpp       # Backtesting tool
│   └── optimize_strategies.cpp # Parameter optimizer
├── tests/                     # 22 test suites
└── benchmarks/
    └── bench_orderbook.cpp    # Performance benchmarks

rust/                          # Rust implementation
├── src/
│   ├── lib.rs
│   ├── types.rs
│   ├── orderbook.rs           # 11 tests
│   └── bin/bench_orderbook.rs
└── Cargo.toml
```

## Benchmark Results

### OrderBook (C++ vs Rust)

| Operation | C++ | Rust |
|-----------|-----|------|
| Cancel Order | 447 ns | 613 ns |
| Execute Order | 486 ns | 542 ns |
| Best Bid/Ask | 19 ns | ~0 ns |
| Throughput | 2.21M ops/sec | 1.44M ops/sec |

### IPC Overhead (Shared Memory)

| Operation | Slow Path | Fast Path | Relaxed | Notes |
|-----------|-----------|-----------|---------|-------|
| Price update | 90 cycles | 17 cycles | **1.5 cycles** | ~0.5ns with relaxed ordering |
| Position update | 120 cycles | 54 cycles | **2.9 cycles** | ~1ns with relaxed ordering |
| Heartbeat | 75 cycles | - | - | Once per second |
| Config read | 9 cycles | - | - | Atomic load |

**Optimization techniques:**
- Direct index access (O(1)) vs string search (O(n))
- `memory_order_relaxed` for single-writer scenarios
- `store(++seq)` vs `fetch_add(1)` (no RMW needed)
- Pre-scaled int64 to avoid double→int conversion

## HFT Lifecycle Management

The HFT engine uses shared memory for process lifecycle:
- **Heartbeat**: Updated every second, dashboard detects stale heartbeat (>3s)
- **Version check**: Git commit hash embedded at compile time, auto-invalidates old shared memory
- **Graceful shutdown**: Signal handlers (SIGTERM/SIGINT) set status before exit

## Next Steps
1. End-to-end simulation with sample ITCH data
2. Optimize Rust implementation (use BTreeMap, arena allocator)
3. Add WebSocket support for remote dashboard

## References
- NASDAQ ITCH 5.0 Spec: https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHspecification.pdf
- C++ Core Guidelines: https://isocpp.github.io/CppCoreGuidelines/
