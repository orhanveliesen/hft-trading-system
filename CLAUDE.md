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
│   ├── benchmark/
│   │   ├── timer.hpp          # RDTSC timing
│   │   └── histogram.hpp      # Latency histogram
│   └── strategy/
│       ├── position.hpp       # Position & P&L tracking
│       ├── risk_manager.hpp   # Risk limits
│       └── market_maker.hpp   # Market making strategy
├── src/
│   ├── orderbook/orderbook.cpp
│   └── benchmark/timer.cpp
├── tests/                     # 42 tests
│   ├── test_orderbook.cpp     # 11 tests
│   ├── test_feed_handler.cpp  # 6 tests
│   ├── test_network.cpp       # 7 tests
│   ├── test_integration.cpp   # 7 tests
│   └── test_strategy.cpp      # 11 tests
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

## Benchmark Results (C++ vs Rust)

| Operation | C++ | Rust |
|-----------|-----|------|
| Cancel Order | 447 ns | 613 ns |
| Execute Order | 486 ns | 542 ns |
| Best Bid/Ask | 19 ns | ~0 ns |
| Throughput | 2.21M ops/sec | 1.44M ops/sec |

## Next Steps
1. End-to-end simulation with sample ITCH data
2. Optimize Rust implementation (use BTreeMap, arena allocator)

## References
- NASDAQ ITCH 5.0 Spec: https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHspecification.pdf
- C++ Core Guidelines: https://isocpp.github.io/CppCoreGuidelines/
