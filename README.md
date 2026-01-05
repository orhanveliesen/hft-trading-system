# HFT Trading System

A high-performance, low-latency trading system implementation in C++ designed for high-frequency trading (HFT) applications.

## Features

### Core Infrastructure
- **Order Book**: O(1) add/cancel/execute operations with pre-allocated memory pools (~160MB)
- **Lock-free Data Structures**: SPSC ring buffers for market data with ~50ns latency
- **Template-based Polymorphism**: Zero vtable overhead for hot paths
- **Memory Efficiency**: Cache-line aligned structures, intrusive linked lists

### Protocol Support
- **ITCH 5.0**: NASDAQ market data protocol parser
- **OUCH 4.2**: NASDAQ order entry protocol with SoupBinTCP framing
- **Binance WebSocket**: Real-time market data and order execution

### Trading Strategies
- Market Making with dynamic spread adjustment
- Mean Reversion with Bollinger Bands
- Momentum/Trend Following
- VWAP Execution
- Pairs Trading
- Order Flow Imbalance

### Regime Detection & Adaptive Trading
- Automatic market regime detection (Trending, Ranging, High/Low Volatility)
- Adaptive strategy selection based on market conditions
- Confidence-weighted position sizing

### Paper Trading
- **Pessimistic Queue-Based Fill Simulation**: Realistic fill detection
- Queue position tracking with FIFO simulation
- Latency and slippage modeling
- Live dashboard with real-time P&L

### Risk Management
- Position limits per symbol
- Maximum drawdown controls
- Automatic trading halt on risk breach
- Order rate limiting

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                     Market Data Feed                         │
│  (UDP Multicast / WebSocket)                                │
└─────────────────────┬───────────────────────────────────────┘
                      │
┌─────────────────────▼───────────────────────────────────────┐
│                   Feed Handler                               │
│  (ITCH Parser / Binance Decoder)                            │
└─────────────────────┬───────────────────────────────────────┘
                      │
┌─────────────────────▼───────────────────────────────────────┐
│                    Order Book                                │
│  (Pre-allocated pools, O(1) operations)                     │
└─────────────────────┬───────────────────────────────────────┘
                      │
┌─────────────────────▼───────────────────────────────────────┐
│               Strategy Engine                                │
│  ┌─────────────┐ ┌─────────────┐ ┌─────────────┐           │
│  │   Regime    │ │  Strategy   │ │    Risk     │           │
│  │  Detector   │ │  Selector   │ │   Manager   │           │
│  └─────────────┘ └─────────────┘ └─────────────┘           │
└─────────────────────┬───────────────────────────────────────┘
                      │
┌─────────────────────▼───────────────────────────────────────┐
│                  Order Sender                                │
│  (OUCH / Binance API / Paper Trading)                       │
└─────────────────────────────────────────────────────────────┘
```

## Performance

| Operation | Latency |
|-----------|---------|
| Order Book Add | ~450 ns |
| Order Book Cancel | ~450 ns |
| Order Book Execute | ~490 ns |
| Best Bid/Ask Query | ~20 ns |
| Async Logging | ~50 ns |
| Ring Buffer Push/Pop | ~30 ns |

## Build

### Requirements
- CMake 3.16+
- C++17 compatible compiler (GCC 9+, Clang 10+)
- Linux (for epoll-based networking)

### Build Commands

```bash
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)

# Run tests
ctest --output-on-failure

# Run benchmarks
./bench_orderbook
```

### Rust Implementation

```bash
cd rust
cargo build --release
cargo test
./target/release/bench_orderbook
```

## Project Structure

```
hft/
├── include/
│   ├── orderbook.hpp          # Order book implementation
│   ├── types.hpp              # Core types (Order, Price, Side)
│   ├── feed_handler.hpp       # ITCH 5.0 parser
│   ├── ouch/                  # OUCH 4.2 protocol
│   ├── paper/                 # Paper trading engine
│   ├── strategy/              # Trading strategies
│   ├── exchange/              # Exchange connectors
│   └── network/               # UDP/WebSocket handlers
├── src/                       # Implementation files
├── tests/                     # Test suites (19 suites)
├── benchmarks/                # Performance benchmarks
├── tools/                     # CLI tools and demos
├── rust/                      # Rust implementation
└── fpga/                      # FPGA RTL designs (Verilog)
```

## Usage Examples

### Paper Trading Demo

```bash
cd build
./paper_trading_demo --duration=60
```

### Backtest with Historical Data

```bash
./run_backtest --strategy=mean_reversion --data=btcusdt_1m.csv
```

## Testing

The project includes comprehensive test coverage:

```bash
ctest --output-on-failure
```

```
19/19 tests passed
- OrderBookTests
- FeedHandlerTests
- NetworkTests
- IntegrationTests
- StrategyTests
- MatchingEngineTests
- PaperTradingTests
- QueueFillDetectorTests
- ... and more
```

## Future Work

- [ ] Unified paper/live trading mode switch
- [ ] Full Binance live integration
- [ ] Advanced risk management (VaR, Greeks)
- [ ] Machine learning signal generation
- [ ] FPGA acceleration for order book

## License

This project is for educational purposes.

## Author

Orhan Veli Esen
