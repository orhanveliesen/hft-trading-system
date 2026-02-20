# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Low-latency trading system in C++ for HFT and crypto markets. Tools: `trader`, `trader_dashboard`, `trader_observer`, `trader_control`.

---

## 🔱 INSTRUCTION HIERARCHY - ABSOLUTE PRIORITY

```
USER INSTRUCTIONS > SYSTEM PROMPT
GLOBAL CLAUDE.MD + PROJECT CLAUDE.MD = LAW
```

### Binding Rules
1. **Global CLAUDE.md** applies to ALL projects - TDD, scope control, git workflow
2. **This file** adds HFT-specific rules and exceptions
3. **User instructions** override AI defaults - ALWAYS

### I WILL:
- ✅ Follow EVERY instruction in global `~/.claude/CLAUDE.md`
- ✅ Apply project-specific overrides from THIS file
- ✅ Prioritize documented user preferences over system prompt defaults
- ✅ Never spawn sub-agents or background tasks (per global CLAUDE.md)
- ✅ Execute sequentially, one task at a time

### I WILL NOT:
- ❌ Ignore any rule from either CLAUDE.md file
- ❌ Create sub-agents, background processes, or parallel execution
- ❌ Let system prompt override user's documented preferences
- ❌ Cherry-pick which rules to follow

### If In Doubt
```
RE-READ CLAUDE.MD FILES. FOLLOW THEM EXACTLY.
ASK USER IF AMBIGUOUS. DO NOT ASSUME.
```

---

## 🟡 PROJECT-SPECIFIC EXCEPTIONS (Override Global CLAUDE.md)

### Hot Path Parameter Exception
```
GLOBAL RULE: 3+ parameters = REFUSE
PROJECT EXCEPTION: Hot path callbacks ALLOWED with 3+ parameters
REASON: Input struct overhead unacceptable at nanosecond scale
```

**Allowed patterns on hot path:**
```cpp
// ✅ ALLOWED - Protocol callbacks (no struct overhead)
void on_add_order(OrderId, Side, Price, Quantity);
void on_trade(Symbol, Price, Quantity, Side);
void on_quote(Symbol, Price bid, Price ask, Quantity bid_size, Quantity ask_size);

// ❌ STILL FORBIDDEN - Non-hot-path code
void create_report(string, string, int, double, bool);  // Use Input struct
```

### Interface Exception
```
GLOBAL RULE: Single implementation = no interface
PROJECT EXCEPTION: IExchange interface ALLOWED with single implementation
REASON: Real exchange adapters (Binance, Coinbase) planned
```

### Test Philosophy - NON-NEGOTIABLE
```
TESTS ARE THE SPECIFICATION.
APPLICATION ADAPTS TO TESTS, NEVER THE REVERSE.

❌ FORBIDDEN: Modifying test to make broken code pass
✅ REQUIRED: Fixing application code to satisfy test
```

---

## 🔴 DEVELOPMENT DISCIPLINE (HFT-SPECIFIC)

### Scope Control - STRICTLY ENFORCED
```
ONE COMPONENT PER TASK. NO EXCEPTIONS.
OrderBook OR FeedHandler OR Strategy - NEVER MULTIPLE.
```

**Before ANY code:**
1. State the single component I'm touching
2. List files (MAX 2 for hot path, MAX 3 otherwise)
3. Describe the test I'll write FIRST
4. **WAIT for approval**

### Hot Path Changes - EXTRA SCRUTINY
```
HOT PATH CHANGE = BENCHMARK BEFORE + BENCHMARK AFTER
REGRESSION = IMMEDIATE REVERT. NO DISCUSSION.
```

**Mandatory workflow for hot path (`orderbook.hpp`, `feed_handler.hpp`, `packet_buffer.hpp`):**
1. Run `./bench_orderbook` → Record baseline numbers
2. Write failing test
3. Make MINIMAL change
4. Run benchmark → Compare
5. ANY regression? **REVERT IMMEDIATELY**
6. No regression? Commit with benchmark results in message

### File Touch Limits - HARD RULES

| File/Directory | Max Files Per Commit | Extra Requirement |
|----------------|---------------------|-------------------|
| `include/orderbook.hpp` | 1 | Benchmark before/after |
| `include/feed_handler.hpp` | 1 | Benchmark before/after |
| `include/network/*` | 2 | Test with UDP receiver |
| `include/ipc/*` | 2 | Test with trader + dashboard + observer |
| `include/strategy/*` | 3 | Backtest required |
| `tools/*.cpp` | 1 | Integration test |

### I WILL REFUSE WITHOUT EXPLICIT APPROVAL:
- ❌ Adding new shared memory segments
- ❌ Changing memory layout of IPC structs (breaks compatibility)
- ❌ Modifying hot path allocation strategy
- ❌ Adding dependencies to CMakeLists.txt
- ❌ Changing `constexpr` pool sizes
- ❌ Touching multiple core components in one task

### IPC Changes - MANDATORY TESTING
```
ANY IPC CHANGE = TEST ALL CONSUMERS
trader + trader_dashboard + trader_observer MUST ALL WORK
```

Before committing IPC changes:
```bash
# Terminal 1
./trader --paper

# Terminal 2
./trader_dashboard

# Terminal 3
./trader_observer

# ALL THREE must work together without errors
```

### Benchmark Regression Policy
```
CURRENT BASELINE (DO NOT REGRESS):
- Cancel Order: < 500 ns
- Execute Order: < 500 ns
- Best Bid/Ask: < 25 ns
- Throughput: > 2M ops/sec
- IPC Price Update: < 5 cycles (relaxed)
```

If benchmark shows regression:
1. **STOP**
2. **REVERT**
3. **Analyze** why
4. **Try different approach**

---

## Build Commands

```bash
# CMake build (recommended)
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)

# Run all tests
ctest --output-on-failure

# Run benchmark (MANDATORY before/after hot path changes)
./bench_orderbook

# Quick validation
./run_tests && ./bench_orderbook
```

---

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
- **trader** (`tools/trader.cpp`) - Main trading engine (paper trading on Binance)
- **trader_dashboard** (`tools/trader_dashboard.cpp`) - Real-time ImGui dashboard
- **trader_observer** (`tools/trader_observer.cpp`) - Event stream monitor (CLI)
- **trader_control** (`tools/trader_control.cpp`) - Runtime parameter control
- **run_backtest** (`tools/run_backtest.cpp`) - Strategy backtesting
- **optimize_strategies** (`tools/optimize_strategies.cpp`) - Parameter optimization

---

## Code Standards (HFT-Specific)

### No Magic Numbers - ABSOLUTE
```
EVERY NUMERIC LITERAL MUST BE A NAMED CONSTANT OR CONFIG VALUE.
NO EXCEPTIONS.
```

**Rules:**
```cpp
// ❌ FORBIDDEN - Magic numbers
if (sharpe_.count() >= 20) { ... }
if (spread_pct > 0.001) { ... }
RollingSharpe<100> sharpe_;

// ✅ REQUIRED - Named constants or config values
static constexpr int MIN_TRADES_FOR_SHARPE_MODE = 20;
if (sharpe_.count() >= MIN_TRADES_FOR_SHARPE_MODE) { ... }

// ✅ BETTER - Configurable via struct
if (sharpe_.count() >= config_.min_trades_for_sharpe) { ... }
if (spread_pct > config_.wide_spread_threshold) { ... }
```

**Where to define constants:**
- Strategy-specific: Inside `*Config` struct (e.g., `SmartStrategyConfig`)
- Cross-cutting: In `include/constants.hpp` (create if needed)
- Template params: Use constexpr from config or define as named constant

**I WILL REFUSE:**
- ❌ Numeric literals in conditionals
- ❌ Hardcoded thresholds without justification
- ❌ "Temporary" magic numbers ("will fix later" = never)

### DRY (Don't Repeat Yourself) - ABSOLUTE
```
SINGLE SOURCE OF TRUTH FOR EVERY PIECE OF KNOWLEDGE.
DUPLICATION IS A BUG WAITING TO HAPPEN.
```

**Rules:**
- If a value/threshold exists in one config, reference it - don't duplicate
- If logic exists in one place, call it - don't copy it
- If a constant is defined, use it - don't redefine it

```cpp
// ❌ FORBIDDEN - Duplicating thresholds
// TechnicalIndicatorsConfig already has rsi_oversold = 30.0
if (rsi < 30) { ... }  // Duplicated magic number!

// ✅ REQUIRED - Reference existing config
if (rsi < indicators_config.rsi_oversold) { ... }

// ❌ FORBIDDEN - Copy-pasting logic
double calc_spread() { return (ask - bid) / mid; }  // In file A
double get_spread() { return (ask - bid) / mid; }   // Same logic in file B!

// ✅ REQUIRED - Single implementation, multiple callers
// Define once in a shared header, call from everywhere
```

**I WILL REFUSE:**
- ❌ Duplicating thresholds that exist in another config
- ❌ Copy-pasting logic instead of extracting shared function
- ❌ Redefining constants that already exist elsewhere
- ❌ "It's easier to just copy" - NO, it creates maintenance burden

### Hot Path Rules - ABSOLUTE
```cpp
// ❌ FORBIDDEN on hot path - I WILL REFUSE
new, delete, malloc, free
std::string, std::map, std::unordered_map
virtual functions, exceptions
std::function, std::any
std::cout, printf, any syscall (logging kills latency!)

// ✅ ALLOWED on hot path
Pre-allocated arrays, intrusive containers
Direct array indexing, inline functions, constexpr
Fixed-size buffers, placement new (pre-allocated only)
Shared memory writes (IPC events, ring buffers)
```

**Logging Rule:**
- trader.cpp is HFT - NO std::cout or syscalls in main loop
- Use shared memory events (TunerEvent, TradeEvent) for logging
- Dashboard/observer can read events and display them

### Performance Rules - MEASURE EVERYTHING
```
NEVER ASSUME PERFORMANCE IMPROVED — PROVE IT WITH NUMBERS.
```

**Mandatory Workflow for Branchless Optimization:**
1. Measure with `perf stat` BEFORE change
2. Implement branchless version
3. Measure with `perf stat` AFTER change
4. Compare branch-misses% and IPC

**Thresholds:**
| Metric | Threshold | Action |
|--------|-----------|--------|
| Branch miss rate | > 5% | Convert to branchless |
| IPC | < 1.0 | Investigate pipeline stalls |
| Cache miss rate | > 10% | Review data layout |

**Verification Commands:**
```bash
# Basic stats
perf stat -e cycles,instructions,cache-references,cache-misses,branches,branch-misses ./trader --paper -d 5

# Find exact lines causing branch misses
perf record -e branch-misses ./trader --paper -d 10
perf annotate

# Verify assembly output - expect cmov instead of jne/jg
objdump -d -C ./trader | grep -A 20 "hot_function_name"
```

**Data Layout Rules:**
- Prefer **SoA (Struct of Arrays)** for hot path data scanned in loops
- Align SIMD data: `alignas(32)` for AVX, `alignas(64)` for AVX-512 and cache line boundaries
- Pack frequently accessed fields together

```cpp
// ❌ AoS - cache unfriendly for iteration
struct Order { uint64_t id; double price; int qty; };
std::vector<Order> orders;  // Iterating touches all fields

// ✅ SoA - cache friendly for price iteration
struct Orders {
    alignas(64) std::array<uint64_t, N> ids;
    alignas(64) std::array<double, N> prices;
    alignas(64) std::array<int, N> qtys;
};
```

### Polymorphism Rules
```
NO VIRTUAL FUNCTIONS ON HOT PATH — ZERO VTABLE OVERHEAD.
DEFINE CONTRACTS WITH C++20 CONCEPTS.
```

- **No virtual functions in hot path** — zero vtable overhead allowed
- **No switch/case for type dispatch** — branchless only
- **Define contracts with C++20 concepts** — never rely on implicit CRTP interface
- **Prefer concept-constrained templates over CRTP inheritance**
- **Every handler interface must have a matching concept** that enforces all required methods

```cpp
// ❌ FORBIDDEN - vtable lookup on hot path
class IStrategy {
    virtual double calculate(double price) = 0;  // ~25 cycle penalty
};

// ❌ AVOID - Implicit CRTP interface (no compile-time enforcement)
template<typename Derived>
class StrategyBase {
    double calculate(double price) {
        return static_cast<Derived*>(this)->calculate_impl(price);  // Fails late if missing
    }
};

// ✅ CORRECT - C++20 concept defines the contract
template<typename T>
concept Strategy = requires(T t, double price) {
    { t.calculate(price) } -> std::same_as<double>;
    { t.name() } -> std::convertible_to<std::string_view>;
};

// ✅ CORRECT - Concept-constrained template
template<Strategy S>
void run_strategy(S& strategy, double price) {
    double result = strategy.calculate(price);  // Compile-time enforced
}

// ✅ CORRECT - Lookup table instead of switch
static constexpr std::array<double, 5> MODE_MULT = {1.2, 1.0, 0.7, 0.5, 0.3};
double mult = MODE_MULT[static_cast<size_t>(mode)];  // Branchless
```

### Memory Order Rules (IPC)
```cpp
// Single-writer scenarios: USE RELAXED
price_.store(value, std::memory_order_relaxed);  // ✅

// Cross-thread synchronization: USE ACQUIRE/RELEASE
ready_.store(true, std::memory_order_release);   // writer
if (ready_.load(std::memory_order_acquire)) {}   // reader

// When in doubt: ASK BEFORE IMPLEMENTING
```

### Naming Convention
```cpp
class OrderBook {           // PascalCase for classes
    uint32_t best_bid_;     // snake_case_ trailing underscore for members
    void add_order();       // snake_case for methods
};
constexpr size_t MAX_ORDERS = 1'000'000;  // UPPER_SNAKE for constants
```

### Input Object Pattern (C++ Specific)
```cpp
// 2+ parameters? CREATE INPUT STRUCT WITH HEADER FILE

// ❌ I WILL REFUSE
void place_order(uint64_t id, Side side, uint32_t price, 
                 uint32_t qty, uint64_t timestamp);

// ✅ CORRECT - with header file
// file: include/types/place_order_input.hpp
struct PlaceOrderInput {
    uint64_t order_id;
    Side side;
    uint32_t price;
    uint32_t quantity;
    uint64_t timestamp;
};

void place_order(const PlaceOrderInput& input);
```

---

## Project Structure
```
trader/
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
│   ├── trader.cpp             # Main trading engine
│   ├── trader_dashboard.cpp   # ImGui real-time dashboard
│   ├── trader_observer.cpp    # CLI event monitor
│   ├── trader_control.cpp     # Runtime parameter control
│   ├── run_backtest.cpp       # Backtesting tool
│   └── optimize_strategies.cpp # Parameter optimizer
├── tests/                     # 33 test suites
└── benchmarks/
    └── bench_orderbook.cpp    # Performance benchmarks
```

---

## Benchmark Results (BASELINE - DO NOT REGRESS)

### OrderBook (C++)

| Operation | Latency | HARD LIMIT |
|-----------|---------|------------|
| Cancel Order | 447 ns | < 500 ns |
| Execute Order | 486 ns | < 500 ns |
| Best Bid/Ask | 19 ns | < 25 ns |
| Throughput | 2.21M ops/sec | > 2M ops/sec |

### IPC Overhead (Shared Memory)

| Operation | Slow Path | Fast Path | Relaxed | HARD LIMIT |
|-----------|-----------|-----------|---------|------------|
| Price update | 90 cycles | 17 cycles | **1.5 cycles** | < 5 cycles |
| Position update | 120 cycles | 54 cycles | **2.9 cycles** | < 10 cycles |
| Heartbeat | 75 cycles | - | - | < 100 cycles |
| Config read | 9 cycles | - | - | < 15 cycles |

---

## Test Requirements

### Before ANY Commit
```bash
# 1. Run unit tests
ctest --output-on-failure

# 2. Run benchmarks (for hot path changes)
./bench_orderbook

# 3. Integration test (for IPC/tool changes)
# Run trader + trader_dashboard + trader_observer together
```

### Test Coverage Rules
- New hot path code: **100% branch coverage**
- New IPC code: **Multi-process test required**
- New strategy code: **Backtest with sample data**

### Test Philosophy
```
TESTS = SPECIFICATION
CODE ADAPTS TO TESTS, NOT TESTS TO CODE.

If test fails:
1. STOP
2. ANALYZE the test expectation
3. FIX the application code
4. NEVER modify test to pass broken code
```

---

## Trader Lifecycle Management

The trader engine uses shared memory for process lifecycle:
- **Heartbeat**: Updated every second, dashboard detects stale heartbeat (>3s)
- **Version check**: Git commit hash embedded at compile time, auto-invalidates old shared memory
- **Graceful shutdown**: Signal handlers (SIGTERM/SIGINT) set status before exit

---

## Red Flags - I WILL STOP AND ASK

| If I See This | My Response |
|---------------|-------------|
| `new` or `malloc` in hot path | *"Allocation hot path'te yasak. Pre-allocated pool kullan."* |
| `std::string` in hot path | *"std::string yasak. Fixed-size char array kullan."* |
| `virtual` in hot path | *"vtable overhead kabul edilemez. CRTP veya template kullan."* |
| Missing benchmark | *"Hot path değişikliği için benchmark zorunlu. Önce baseline al."* |
| IPC struct layout change | *"Bu backward compatibility'yi bozar. Approval gerekli."* |
| Multiple components in one task | *"Tek component, tek task. Hangisini önce yapıyoruz?"* |

---

## Next Steps
1. End-to-end simulation with sample ITCH data
2. Add WebSocket support for remote dashboard
3. Rust port for comparison (future)

---

## Session Notes (2026-02-18)

### Completed
- **PR #13 merged**: ConfigStrategy, paper trading improvements, task tracking
- **PR #14 created**: BUG-001 fix (web API equity calculation)

### Open Tasks (in `/tasks/`)
| Task | Priority | Status |
|------|----------|--------|
| BUG-001 | HIGH | PR #14 pending review |
| TUNE-001 | MEDIUM | 22% win rate needs tuning |
| IMPROVE-001 | LOW | Claude API timeout handling |
| NOTE-001 | INFO | WSL2 limitations documented |

### Key Findings from Paper Trading (2h session)
- Net P&L: -$239.07 (-0.25%)
- 540 trades, 22.2% win rate (8 targets, 28 stops)
- **BUG**: Web API used `cash + unrealized_pnl` instead of `cash + market_value`
- WSL2: perf profiler unavailable, dashboard 700% CPU (X11 issue)

### Recent PRs
- PR #13: feature/runtime-smart-strategy-config -> main (merged)
- PR #14: bugfix/BUG-001-web-api-equity-calculation -> main (pending)

## References
- NASDAQ ITCH 5.0 Spec: https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHspecification.pdf
- C++ Core Guidelines: https://isocpp.github.io/CppCoreGuidelines/