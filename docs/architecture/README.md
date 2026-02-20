# HFT Trading System - Architecture Documentation

This directory contains UML diagrams documenting the architecture of the HFT trading system.

## Diagrams

### 1. Class Diagram (`class-diagram.puml`)

Shows the static structure of the system including:

- **Core Types**: `Order`, `PriceLevel`, `Trade`, `Side`, `OrderResult`
- **Order Book**: `OrderBook`, `BookSide<Compare>`, `BidSide`, `AskSide`
- **Network**: `UdpReceiver`, `PacketBuffer`, `MoldUDP64Header`
- **Feed Handler**: `FeedHandler<Callback>`, `FeedCallback` concept
- **IPC**: `SharedConfig`, `SharedRingBuffer`, `TradeEvent`, `SharedPortfolioState`
- **Strategy**: `SmartStrategy`, `RegimeDetector`, `TechnicalIndicators`, `RiskManager`
- **Paper Trading**: `PaperTradingEngine`, `PaperOrderSender`
- **Trading Application**: `TradingApp`, `ExecutionEngine`, `Portfolio`, `EventPublisher`
- **C++20 Concepts**: `OrderSender`, `TradingStrategy`, `RiskChecker`, `ReadableOrderBook`

### 2. Sequence Diagrams (`sequence-diagrams.puml`)

Contains 6 sequence diagrams showing runtime behavior:

1. **Market Data Flow (ITCH Protocol)**: UDP multicast → FeedHandler → OrderBook
2. **Paper Trading Signal to Fill**: Quote → Strategy → Risk Check → Order → Fill
3. **Position Exit Flow**: Target Hit / Stop Loss handling
4. **IPC Communication**: Shared memory producer/consumer patterns
5. **Strategy Mode Transitions**: NORMAL → AGGRESSIVE → CAUTIOUS → DEFENSIVE → EXIT_ONLY
6. **Order Book Operations**: Add, Execute, Cancel with pool management

### 3. Deployment Diagram (`deployment-diagram.puml`)

Shows the physical deployment including:

- **External Systems**: Binance Exchange (WebSocket/REST), NASDAQ (MoldUDP64/OUCH)
- **Trading Server**:
  - `trader` (HFT Engine) - main trading process
  - `trader_dashboard` (ImGui) - real-time GUI monitoring
  - `trader_observer` (CLI) - terminal-based event monitoring
  - `trader_control` (CLI) - runtime configuration
  - `trader_tuner` (Optional) - auto-parameter tuning
- **Shared Memory Regions**: Event buffers, portfolio state, config, ledger
- **Component Interaction**: Data flow between components
- **Memory Layout**: Pre-allocated pools visualization

## Rendering the Diagrams

### Using PlantUML CLI

```bash
# Install PlantUML (requires Java)
sudo apt install plantuml

# Render all diagrams to PNG
plantuml -tpng docs/architecture/*.puml

# Render to SVG (recommended for documentation)
plantuml -tsvg docs/architecture/*.puml
```

### Using VS Code

1. Install the "PlantUML" extension
2. Open any `.puml` file
3. Press `Alt+D` to preview

### Using Online Renderer

Copy the PlantUML code to: https://www.plantuml.com/plantuml/uml/

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                           EXTERNAL SYSTEMS                                   │
├─────────────────────────────────────────────────────────────────────────────┤
│  ┌─────────────────┐    ┌─────────────────┐    ┌─────────────────┐         │
│  │ Binance WS      │    │ Binance REST    │    │ NASDAQ MoldUDP  │         │
│  │ (price ticks)   │    │ (order place)   │    │ (future)        │         │
│  └────────┬────────┘    └────────┬────────┘    └────────┬────────┘         │
└───────────┼─────────────────────┼─────────────────────┼────────────────────┘
            │                      │                      │
            ▼                      ▼                      ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                           TRADING ENGINE (trader)                            │
├─────────────────────────────────────────────────────────────────────────────┤
│  ┌──────────────┐   ┌──────────────┐   ┌──────────────┐   ┌──────────────┐ │
│  │ FeedHandler  │──▶│  OrderBook   │──▶│SmartStrategy │──▶│ Execution    │ │
│  │ (parse)      │   │ (160MB pool) │   │ (signals)    │   │ Engine       │ │
│  └──────────────┘   └──────────────┘   └──────────────┘   └──────────────┘ │
│         │                  │                  │                  │          │
│         │           ┌──────┴──────┐    ┌──────┴──────┐    ┌──────┴──────┐  │
│         │           │ BidSide     │    │ Regime      │    │ Portfolio   │  │
│         │           │ AskSide     │    │ Detector    │    │ Risk Mgr    │  │
│         │           └─────────────┘    └─────────────┘    └─────────────┘  │
│         │                                                        │          │
│         │                                                        ▼          │
│  ┌──────┴──────────────────────────────────────────────────────────────┐   │
│  │                     EVENT PUBLISHER                                  │   │
│  │              (push to SharedRingBuffer)                             │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                        POSIX SHARED MEMORY                                   │
├─────────────────────────────────────────────────────────────────────────────┤
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐        │
│  │/hft_events  │  │/trader_     │  │/trader_     │  │/trader_     │        │
│  │(ring buffer)│  │portfolio    │  │config       │  │ledger       │        │
│  │64K events   │  │(state)      │  │(hot reload) │  │(trades)     │        │
│  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘  └─────────────┘        │
└─────────┼────────────────┼────────────────┼────────────────────────────────┘
          │                │                │
          ▼                ▼                ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                         MONITORING PROCESSES                                 │
├─────────────────────────────────────────────────────────────────────────────┤
│  ┌─────────────────┐    ┌─────────────────┐    ┌─────────────────┐         │
│  │ trader_observer │    │ trader_dashboard│    │ trader_control  │         │
│  │ (CLI events)    │    │ (ImGui GUI)     │    │ (config CLI)    │         │
│  │ read events     │    │ read portfolio  │    │ write config    │         │
│  └─────────────────┘    └─────────────────┘    └─────────────────┘         │
└─────────────────────────────────────────────────────────────────────────────┘
```

## Key Design Decisions

### 1. Pre-allocated Memory Pools

- **OrderBook**: ~160MB pre-allocated for 1M orders + 100K price levels
- **Zero allocation on hot path**: All memory from pools
- **Intrusive linked lists**: O(1) add/remove without allocation

### 2. Lock-Free IPC

- **SPSC Ring Buffer**: Single-producer single-consumer, no locks
- **Cache-line alignment**: Prevent false sharing (64-byte alignment)
- **Memory ordering**: acquire/release semantics for visibility

### 3. Template-Based Polymorphism

- **No virtual functions on hot path**: Zero vtable overhead
- **C++20 Concepts**: Compile-time interface enforcement
- **CRTP where needed**: Static dispatch

### 4. Strategy Architecture

- **Self-adaptive**: Mode transitions based on performance
- **Risk-aware**: Sharpe ratio-based position sizing
- **Regime-aware**: Different parameters for trending/ranging markets

## Performance Targets

| Operation | Latency | Status |
|-----------|---------|--------|
| Cancel Order | < 500 ns | ✅ 447 ns |
| Execute Order | < 500 ns | ✅ 486 ns |
| Best Bid/Ask | < 25 ns | ✅ 19 ns |
| IPC Price Update | < 5 cycles | ✅ 1.5 cycles |
| Throughput | > 2M ops/sec | ✅ 2.21M ops/sec |

## File Locations

| Component | Header | Source |
|-----------|--------|--------|
| OrderBook | `include/orderbook.hpp` | (header-only) |
| FeedHandler | `include/feed_handler.hpp` | (header-only) |
| SmartStrategy | `include/strategy/smart_strategy.hpp` | (header-only) |
| SharedRingBuffer | `include/ipc/shared_ring_buffer.hpp` | (header-only) |
| Trader | - | `tools/trader.cpp` |
| Dashboard | - | `tools/trader_dashboard.cpp` |
| Observer | - | `tools/trader_observer.cpp` |
