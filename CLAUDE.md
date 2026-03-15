# HFT Trading System

Low-latency C++ trading system for HFT and crypto markets. Uses pre-allocated pools, lock-free IPC, and zero-copy UDP multicast parsing.

## 🏗️ ARCHITECTURE FIRST - MANDATORY

```
BEFORE ANY TASK: READ ARCHITECTURE DOCS FIRST.
UNDERSTAND THE SYSTEM BEFORE CHANGING IT.
```

### Architecture Documentation Location
```
docs/architecture/
├── README.md              # Overview, design decisions, performance targets
├── class-diagram.puml     # Static structure (50+ classes, 8 packages)
├── sequence-diagrams.puml # Runtime behavior (6 diagrams)
└── deployment-diagram.puml # Physical layout, memory pools, IPC
```

### Before Starting ANY Task
1. **Read `docs/architecture/README.md`** - Understand the big picture
2. **Check relevant diagram** - Class diagram for structure, sequence for flow
3. **Identify affected components** - Which packages/classes are involved?
4. **Then start thinking** about the implementation

### I WILL:
- ✅ Read architecture docs before proposing changes
- ✅ Reference diagrams when discussing components
- ✅ Update diagrams if architecture changes significantly
- ✅ Keep mental model aligned with documented architecture

### I WILL NOT:
- ❌ Start coding without understanding system structure
- ❌ Make assumptions about component relationships
- ❌ Forget the pre-allocated pool architecture
- ❌ Ignore the hot path constraints documented in diagrams

---

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
# Build (ALWAYS from project root, NEVER cd into build/)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# Test
ctest --test-dir build --output-on-failure

# Coverage build (separate directory)
cmake -B build-coverage -DCMAKE_BUILD_TYPE=Debug -DENABLE_COVERAGE=ON
cmake --build build-coverage -j$(nproc)
ctest --test-dir build-coverage --output-on-failure

# Generate coverage report (outputs to build-coverage/coverage_html/)
cd build-coverage && cmake --build . --target coverage && cd ..

# Format code (before commit)
find include tools tests benchmarks -type f \( -name "*.cpp" -o -name "*.hpp" \) -exec clang-format -i {} +

# Setup pre-commit hook (auto-format on commit)
git config core.hooksPath .githooks

# Benchmark (mandatory for hot path changes)
./build/bench_orderbook

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

### Checking CI Results After Push
After pushing to a branch, always verify GitHub Actions status:
```bash
# Check CI status for a PR
gh pr checks <pr-number>

# View failed workflow logs
gh run view <run-id> --log

# Or monitor in real-time
gh run watch
```
**Important**: Do not merge until all checks pass (build-test, lint, coverage).

### Code Coverage Requirements
- **Target**: 100% line coverage (strict)
- **Tool**: lcov + gcov
- **Build isolation**: All coverage artifacts (`.info`, `coverage_html/`, `.gcda`, `.gcno`) MUST stay in build directories (`build-coverage/`). NEVER generate coverage reports in project root.

#### Source Code Markers vs Filter Config

**NEVER use source code markers** (`LCOV_EXCL_LINE`, `LCOV_EXCL_START/STOP`):
- Markers hide untested code in plain sight
- New features can be excluded without review
- Coverage percentage lies over time

**ALWAYS use `lcov --remove` filter config** for genuinely untestable code:
- **Single source of truth**: `cmake/coverage_excludes.list`
- All tools read from this file: `cmake/coverage.cmake`, `.githooks/pre-commit`, `.github/workflows/codecov.yml`
- Adding a new exclusion = one line in one file
- Visible in code review, transparent and auditable

```
Marker = coverage'ı gizle (dürüst değil)
Filter = coverage'ı belgeleyerek dışarıda bırak (şeffaf)
```

#### Excluded Patterns (Single Source: `cmake/coverage_excludes.list`)
These patterns are filtered via `lcov --remove`, NOT source markers:

| Pattern | Reason |
|---------|--------|
| `/usr/*` | System headers |
| `*/external/*` | Third-party libraries |
| `*/tests/*` | Test code itself |
| `*_ws.hpp` | WebSocket network I/O (requires integration tests) |
| `*/ipc/shared_*.hpp` | IPC syscall failures (shm_open, mmap, ftruncate) |
| `*/network/udp_*.hpp` | UDP network I/O (requires integration tests) |

**Adding new exclusions**: Add one line to `cmake/coverage_excludes.list`. Requires PR review with justification. Document why the code is untestable, not just inconvenient to test.

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

docker run --rm -v $(pwd):/workspace ghcr.io/orhanveliesen/hft-builder:latest \
  bash -c "cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)"

docker run --rm -v $(pwd):/workspace ghcr.io/orhanveliesen/hft-builder:latest \
  bash -c "ctest --test-dir build --output-on-failure"
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

### Header-Only Metrics Libraries
All metrics classes (`TradeStreamMetrics`, `OrderBookMetrics`, `OrderFlowMetrics`, etc.) are implemented as header-only libraries with inline implementations directly in the `.hpp` files. This enables:
- Zero overhead abstraction (full inlining at compile-time)
- No vtable overhead (no virtual functions)
- Maximum optimization by compiler (link-time optimization across translation units)
- Single-header convenience for users

**Pattern**: Define class interface and implement methods inline in the same `.hpp` file. No separate `.cpp` file.

```cpp
// ✅ CORRECT - Header-only with inline implementation
// include/metrics/order_flow_metrics.hpp
class OrderFlowMetrics {
public:
    void on_trade(const TradeEvent& trade);
    // ... more methods
};

// Implementation in same file
inline void OrderFlowMetrics::on_trade(const TradeEvent& trade) {
    // ... implementation
}
```

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

### Testing and Coverage Rules

**CRITICAL - These rules are MANDATORY:**

1. **NEVER use `--no-verify`**
   - Pre-commit hooks MUST run on every commit
   - If coverage fails, FIX the coverage gap with real tests
   - Bypassing hooks is forbidden - no exceptions

2. **NEVER use `LCOV_EXCL_LINE` or `LCOV_EXCL_START/STOP`**
   - All code must have real test coverage
   - If code cannot be tested, it should not exist in production
   - Network I/O and external dependencies must be mocked/injected, not excluded
   - Exception: Only allowed for code that will be deleted soon (mark with TODO + date)

3. **Test Mocking Pattern: Inheritance-Based**
   ```cpp
   // ✅ CORRECT - Inheritance-based mock
   class BinanceWs {
   public:
       virtual void connect() { /* real network */ }
       virtual ~BinanceWs() = default;
   };

   class MockBinanceWs : public BinanceWs {
   public:
       void connect() override { /* immediate, no network */ }
       void simulate_error(const std::string& msg) { /* test-only */ }
   };

   // Test uses MockBinanceWs
   // Production uses BinanceWs
   ```

   ```cpp
   // ❌ WRONG - Test mode flag in production code
   class BinanceWs {
       bool test_mode_ = false;  // ← Production code polluted with test concern
   public:
       void set_test_mode(bool m) { test_mode_ = m; }  // ← SRP violation
       void connect() {
           if (test_mode_) return;  // ← Runtime branch overhead
           // real network...
       }
   };
   ```

   **Why inheritance-based mocking:**
   - Zero runtime overhead in production (no test_mode_ branches)
   - Clean separation: test concerns stay in test classes
   - SRP: production class does production work only
   - Virtual call overhead negligible for non-hot-path (connect/disconnect)

   **For hot-path (order book, feed handler): use template policy instead:**
   ```cpp
   template<typename NetworkDriver = LiveWebSocketDriver>
   class BinanceWs {
       NetworkDriver driver_;
   public:
       void connect() { driver_.connect(endpoint_, this); }
   };

   // Production: BinanceWs<> (defaults to LiveWebSocketDriver)
   // Test: BinanceWs<MockNetworkDriver>
   ```

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

### Hot Path Optimization Techniques

**Branchless:** Use branchless approaches where possible.

**Pipelining:** Start independent ops first, minimize live variables, keep dependencies close together.

**Single-Pass:** One traversal computing all metrics beats multiple passes. Cache locality > small perf cost.

**Zero-Overhead:** Template callbacks (`template <typename Callback>`) are fully inlined at compile-time. No vtables. Concepts if needed.

**SIMD:** Use SIMD intrinsics (AVX2/AVX-512) for parallel data processing where possible. Vectorize loops operating on arrays.

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
