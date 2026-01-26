# Unified Strategy Architecture Plan

## Mevcut Durum Analizi

### Problemler
1. **Hardcoded Strategy**: trader.cpp sadece TechnicalIndicators kullanıyor
2. **Kullanılmayan Stratejiler**: MarketMaker, FairValue, Momentum vs. sınıflar var ama entegre değil
3. **Akıllı Seçici Atıl**: RegimeDetector var ama strategy seçimi için kullanılmıyor
4. **Paper/Prod Karışık**: Slippage simülasyonu execution layer'a sızmış
5. **Order Type Hardcoded**: Sadece market order, limit order yok

### Mevcut Stratejiler (include/strategy/)
```
├── market_maker.hpp      # İki taraflı quote, limit order doğal
├── fair_value.hpp        # Microprice bazlı, limit uygun
├── momentum.hpp          # Trend takip, market order uygun
├── simple_mean_reversion.hpp  # Mean reversion, limit uygun
├── technical_indicators.hpp   # RSI/MACD/BB, şu an kullanılan
├── pairs_trading.hpp     # Pair arbitraj
├── order_flow_imbalance.hpp   # Order flow bazlı
├── vwap.hpp              # VWAP execution
├── adaptive_strategy.hpp # Adaptive
└── smart_strategy.hpp    # Meta-strategy (regime based)
```

---

## Hedef Mimari

```
┌────────────────────────────────────────────────────────────────┐
│                          trader.cpp                             │
├────────────────────────────────────────────────────────────────┤
│  1. CONFIG LAYER                                                │
│     SharedConfig: runtime params (target%, stop%, cooldown)     │
│     StrategyConfig: which strategies enabled, params            │
├────────────────────────────────────────────────────────────────┤
│  2. STRATEGY LAYER                                              │
│     ┌─────────────────────────────────────────────────────┐    │
│     │              IStrategy (interface)                   │    │
│     │  + Signal generate(MarketData&, Position&)          │    │
│     │  + OrderType preferred_order_type()                 │    │
│     │  + string name()                                    │    │
│     └─────────────────────────────────────────────────────┘    │
│              ↑           ↑           ↑           ↑              │
│     ┌────────┴──┐ ┌──────┴───┐ ┌─────┴────┐ ┌────┴─────┐       │
│     │MarketMaker│ │FairValue │ │ Momentum │ │MeanRev   │       │
│     │(limit)    │ │(limit)   │ │(market)  │ │(limit)   │       │
│     └───────────┘ └──────────┘ └──────────┘ └──────────┘       │
│                                                                 │
│     ┌─────────────────────────────────────────────────────┐    │
│     │           StrategySelector                           │    │
│     │  - RegimeBasedSelector: regime → strategy mapping    │    │
│     │  - ConfigBasedSelector: config'den seçim             │    │
│     │  - CompositeSelector: birden fazla strategy kombine  │    │
│     └─────────────────────────────────────────────────────┘    │
├────────────────────────────────────────────────────────────────┤
│  3. EXECUTION LAYER                                             │
│     ┌─────────────────────────────────────────────────────┐    │
│     │              ExecutionEngine                         │    │
│     │  - Signal → Order dönüşümü                          │    │
│     │  - Limit vs Market karar (signal strength + regime) │    │
│     │  - Order tracking (pending orders)                  │    │
│     │  - Fill handling                                    │    │
│     └─────────────────────────────────────────────────────┘    │
├────────────────────────────────────────────────────────────────┤
│  4. EXCHANGE LAYER (tek interface, iki impl)                    │
│     ┌─────────────────────────────────────────────────────┐    │
│     │              IExchange (interface)                   │    │
│     │  + send_market_order(symbol, side, qty)             │    │
│     │  + send_limit_order(symbol, side, qty, price)       │    │
│     │  + cancel_order(order_id)                           │    │
│     │  + on_fill(callback)                                │    │
│     └─────────────────────────────────────────────────────┘    │
│              ↑                           ↑                      │
│     ┌────────┴──────────┐      ┌─────────┴──────────┐          │
│     │  PaperExchange    │      │   BinanceExchange  │          │
│     │  - Slippage sim   │      │   - Real API       │          │
│     │  - Commission sim │      │   - Real fills     │          │
│     │  - Instant fill   │      │   - WebSocket      │          │
│     └───────────────────┘      └────────────────────┘          │
└────────────────────────────────────────────────────────────────┘
```

---

## Implementation Plan

### Phase 1: Strategy Interface (IStrategy)
**Dosya:** `include/strategy/istrategy.hpp`

```cpp
#pragma once
#include "../types.hpp"
#include <string_view>

namespace trader::strategy {

// Sinyal türleri
enum class SignalType : uint8_t {
    None = 0,
    Buy,
    Sell,
    Exit      // Pozisyonu kapat
};

enum class SignalStrength : uint8_t {
    None = 0,
    Weak,
    Medium,
    Strong
};

enum class OrderPreference : uint8_t {
    Market,   // Hemen execute et
    Limit,    // Passive, slippage'siz
    Either    // Execution engine karar versin
};

// Market data snapshot
struct MarketSnapshot {
    Price bid;
    Price ask;
    Quantity bid_size;
    Quantity ask_size;
    Price last_trade;
    uint64_t timestamp_ns;

    Price mid() const { return (bid + ask) / 2; }
    Price spread() const { return ask - bid; }
    double spread_bps() const { return spread() * 10000.0 / mid(); }
};

// Position info
struct PositionInfo {
    double quantity;        // Current holding
    double avg_entry_price; // Average entry
    double unrealized_pnl;  // Current P&L
    double cash_available;  // For new trades
};

// Strategy output
struct Signal {
    SignalType type = SignalType::None;
    SignalStrength strength = SignalStrength::None;
    OrderPreference order_pref = OrderPreference::Either;
    double suggested_qty = 0;
    Price limit_price = 0;   // For limit orders
    const char* reason = ""; // For logging/debugging
};

// Strategy interface
class IStrategy {
public:
    virtual ~IStrategy() = default;

    // Ana sinyal üretici
    virtual Signal generate(
        Symbol symbol,
        const MarketSnapshot& market,
        const PositionInfo& position,
        MarketRegime regime
    ) = 0;

    // Strategy metadata
    virtual std::string_view name() const = 0;
    virtual OrderPreference default_order_preference() const = 0;

    // Hangi rejimlerde çalışmalı
    virtual bool suitable_for_regime(MarketRegime regime) const = 0;

    // Update internal state (called every tick)
    virtual void on_tick(const MarketSnapshot& market) = 0;

    // Reset state
    virtual void reset() = 0;

    // Ready to generate signals?
    virtual bool ready() const = 0;
};

}  // namespace trader::strategy
```

### Phase 2: Mevcut Stratejileri Adapt Et

**Örnek: TechnicalIndicatorsStrategy**
```cpp
// include/strategy/technical_indicators_strategy.hpp
class TechnicalIndicatorsStrategy : public IStrategy {
public:
    Signal generate(Symbol symbol, const MarketSnapshot& market,
                   const PositionInfo& position, MarketRegime regime) override {
        Signal sig;

        // Mevcut TechnicalIndicators mantığı
        auto buy_str = indicators_.buy_signal();
        auto sell_str = indicators_.sell_signal();

        if (buy_str >= SS::Medium && position.quantity == 0) {
            sig.type = SignalType::Buy;
            sig.strength = to_signal_strength(buy_str);
            sig.order_pref = (buy_str >= SS::Strong)
                ? OrderPreference::Market
                : OrderPreference::Limit;
            sig.suggested_qty = calculate_qty(market, position);
            sig.limit_price = market.mid();  // For limit order
            sig.reason = "RSI oversold + MACD cross";
        }
        // ... sell logic

        return sig;
    }

    std::string_view name() const override { return "TechnicalIndicators"; }
    OrderPreference default_order_preference() const override {
        return OrderPreference::Either;
    }
    bool suitable_for_regime(MarketRegime r) const override {
        return r != MarketRegime::HighVolatility;  // Avoid during chaos
    }

private:
    TechnicalIndicators indicators_;
};
```

**Örnek: MarketMakerStrategy**
```cpp
class MarketMakerStrategy : public IStrategy {
public:
    Signal generate(Symbol symbol, const MarketSnapshot& market,
                   const PositionInfo& position, MarketRegime regime) override {
        Signal sig;

        // Market maker her zaman limit order kullanır
        auto quote = mm_.generate_quotes(market.mid(), position.quantity);

        if (quote.has_bid && position.cash_available > 0) {
            sig.type = SignalType::Buy;
            sig.strength = SignalStrength::Weak;  // Passive
            sig.order_pref = OrderPreference::Limit;  // Always limit
            sig.suggested_qty = quote.bid_size;
            sig.limit_price = quote.bid_price;
            sig.reason = "MM bid quote";
        }
        // ... ask quote logic (for selling)

        return sig;
    }

    std::string_view name() const override { return "MarketMaker"; }
    OrderPreference default_order_preference() const override {
        return OrderPreference::Limit;  // Always limit
    }
    bool suitable_for_regime(MarketRegime r) const override {
        // MM works best in ranging markets
        return r == MarketRegime::Ranging || r == MarketRegime::LowVolatility;
    }

private:
    MarketMaker mm_;
};
```

### Phase 3: Strategy Selector

```cpp
// include/strategy/strategy_selector.hpp
class StrategySelector {
public:
    // Register available strategies
    void register_strategy(std::unique_ptr<IStrategy> strategy) {
        strategies_.push_back(std::move(strategy));
    }

    // Select best strategy for current regime
    IStrategy* select(MarketRegime regime) {
        // Priority: find strategy that's suitable and ready
        for (auto& s : strategies_) {
            if (s->suitable_for_regime(regime) && s->ready()) {
                return s.get();
            }
        }
        return default_strategy_;  // Fallback
    }

    // Config-based selection
    IStrategy* select_by_name(std::string_view name) {
        for (auto& s : strategies_) {
            if (s->name() == name) return s.get();
        }
        return nullptr;
    }

    // Composite: run multiple strategies, combine signals
    Signal composite_signal(MarketRegime regime, Symbol symbol,
                           const MarketSnapshot& market,
                           const PositionInfo& position) {
        std::vector<Signal> signals;
        for (auto& s : strategies_) {
            if (s->suitable_for_regime(regime) && s->ready()) {
                signals.push_back(s->generate(symbol, market, position, regime));
            }
        }
        return combine_signals(signals);  // Voting, weighted average, etc.
    }

private:
    std::vector<std::unique_ptr<IStrategy>> strategies_;
    IStrategy* default_strategy_ = nullptr;
};
```

### Phase 4: Execution Engine

```cpp
// include/execution/execution_engine.hpp
class ExecutionEngine {
public:
    void set_exchange(IExchange* exchange) { exchange_ = exchange; }

    // Signal'i order'a çevir ve gönder
    void execute(Symbol symbol, const Signal& signal,
                 const MarketSnapshot& market, MarketRegime regime) {
        if (signal.type == SignalType::None) return;

        // Order type kararı
        OrderType order_type = decide_order_type(signal, market, regime);

        Side side = (signal.type == SignalType::Buy) ? Side::Buy : Side::Sell;

        if (order_type == OrderType::Market) {
            Price expected = (side == Side::Buy) ? market.ask : market.bid;
            exchange_->send_market_order(symbol, side, signal.suggested_qty, expected);
        } else {
            // Limit order
            Price limit = signal.limit_price;
            if (limit == 0) {
                // Strategy didn't specify, use mid
                limit = market.mid();
            }
            auto order_id = exchange_->send_limit_order(symbol, side,
                                                         signal.suggested_qty, limit);
            track_pending_order(order_id, symbol, side, limit);
        }
    }

private:
    OrderType decide_order_type(const Signal& signal,
                                const MarketSnapshot& market,
                                MarketRegime regime) {
        // Strategy preference
        if (signal.order_pref == OrderPreference::Market) return OrderType::Market;
        if (signal.order_pref == OrderPreference::Limit) return OrderType::Limit;

        // Execution engine decides
        // Strong signal → Market (don't miss it)
        if (signal.strength >= SignalStrength::Strong) {
            return OrderType::Market;
        }

        // High volatility → Market (spread might widen)
        if (regime == MarketRegime::HighVolatility) {
            return OrderType::Market;
        }

        // Wide spread → Limit (save cost)
        if (market.spread_bps() > 10) {  // > 10 bps
            return OrderType::Limit;
        }

        // Default: Limit (save slippage)
        return OrderType::Limit;
    }

    IExchange* exchange_ = nullptr;
    std::vector<PendingOrder> pending_orders_;
};
```

### Phase 5: Exchange Interface

```cpp
// include/exchange/iexchange.hpp
class IExchange {
public:
    virtual ~IExchange() = default;

    using FillCallback = std::function<void(const ExecutionReport&)>;
    using SlippageCallback = std::function<void(double)>;

    // Order operations
    virtual bool send_market_order(Symbol sym, Side side, Quantity qty,
                                   Price expected_price) = 0;
    virtual OrderId send_limit_order(Symbol sym, Side side, Quantity qty,
                                     Price limit_price) = 0;
    virtual bool cancel_order(OrderId id) = 0;

    // Price updates (for limit order fills)
    virtual void on_price_update(Symbol sym, Price bid, Price ask) = 0;

    // Callbacks
    virtual void set_fill_callback(FillCallback cb) = 0;
    virtual void set_slippage_callback(SlippageCallback cb) = 0;  // Paper only

    // Info
    virtual bool is_paper() const = 0;
    virtual size_t pending_order_count() const = 0;
};

// Paper implementation (mevcut PaperExchange adapt edilir)
class PaperExchange : public IExchange {
    bool is_paper() const override { return true; }
    // ... slippage simulation, instant fills, etc.
};

// Production implementation
class BinanceExchange : public IExchange {
    bool is_paper() const override { return false; }
    // ... real API calls, WebSocket fills, etc.
};
```

### Phase 6: trader.cpp Refactor

```cpp
// Yeni trader.cpp yapısı (simplified)
template<typename Exchange>
class TradingApp {
public:
    TradingApp(const CLIArgs& args) {
        // 1. Exchange setup
        if constexpr (std::is_same_v<Exchange, PaperExchange>) {
            exchange_.set_slippage_callback([this](double s) {
                portfolio_state_->add_slippage(s);
            });
        }

        // 2. Register strategies
        strategy_selector_.register_strategy(
            std::make_unique<TechnicalIndicatorsStrategy>());
        strategy_selector_.register_strategy(
            std::make_unique<MarketMakerStrategy>());
        strategy_selector_.register_strategy(
            std::make_unique<FairValueStrategy>());
        strategy_selector_.register_strategy(
            std::make_unique<MomentumStrategy>());

        // 3. Set default or config-based strategy
        if (!args.strategy.empty()) {
            active_strategy_ = strategy_selector_.select_by_name(args.strategy);
        }

        // 4. Setup execution engine
        execution_.set_exchange(&exchange_);
    }

    void on_quote(Symbol id, Price bid, Price ask) {
        // Update regime
        auto regime = regime_detector_.update(bid, ask);

        // Select strategy (can change based on regime)
        if (args_.auto_strategy) {
            active_strategy_ = strategy_selector_.select(regime);
        }

        // Build market snapshot
        MarketSnapshot market{bid, ask, /* sizes */, /* timestamp */};

        // Build position info
        PositionInfo position{
            portfolio_.get_holding(id),
            portfolio_.avg_entry_price(id),
            portfolio_.unrealized_pnl(id),
            portfolio_.cash
        };

        // Get signal from strategy
        Signal signal = active_strategy_->generate(id, market, position, regime);

        // Execute
        execution_.execute(id, signal, market, regime);
    }

private:
    Exchange exchange_;
    StrategySelector strategy_selector_;
    IStrategy* active_strategy_ = nullptr;
    ExecutionEngine execution_;
    RegimeDetector regime_detector_;
    Portfolio portfolio_;
};

// main()
int main(int argc, char** argv) {
    auto args = parse_args(argc, argv);

    if (args.paper_mode) {
        TradingApp<PaperExchange> app(args);
        return app.run();
    } else {
        TradingApp<BinanceExchange> app(args);
        return app.run();
    }
}
```

---

## Migration Plan (Adım Adım)

### Adım 1: IStrategy Interface (1-2 saat)
- [ ] `include/strategy/istrategy.hpp` oluştur
- [ ] Signal, MarketSnapshot, PositionInfo struct'ları
- [ ] IStrategy abstract class

### Adım 2: TechnicalIndicatorsStrategy Adapter (2 saat)
- [ ] Mevcut TechnicalIndicators'ı IStrategy'ye wrap et
- [ ] trader.cpp'de test et (mevcut davranış korunmalı)

### Adım 3: MarketMakerStrategy Adapter (1 saat)
- [ ] MarketMaker'ı IStrategy'ye wrap et
- [ ] Limit order desteği

### Adım 4: StrategySelector (2 saat)
- [ ] Strategy registration
- [ ] Regime-based selection
- [ ] Config-based selection (--strategy=market-maker)

### Adım 5: ExecutionEngine (2-3 saat)
- [ ] Signal → Order dönüşümü
- [ ] Limit vs Market karar mekanizması
- [ ] Pending order tracking

### Adım 6: IExchange Interface (2 saat)
- [ ] Abstract interface
- [ ] PaperExchange'i adapt et
- [ ] Slippage callback sadece paper'da

### Adım 7: trader.cpp Refactor (3-4 saat)
- [ ] Yeni yapıya geçiş
- [ ] Backward compatibility (mevcut CLI args çalışmalı)
- [ ] Test

### Adım 8: Diğer Stratejiler (her biri 1 saat)
- [ ] FairValueStrategy
- [ ] MomentumStrategy
- [ ] MeanReversionStrategy
- [ ] PairsTradingStrategy (opsiyonel)

---

## CLI Kullanımı (Hedef)

```bash
# Mevcut davranış (default: technical indicators)
./trader --paper

# Belirli strategy
./trader --paper --strategy=market-maker
./trader --paper --strategy=fair-value
./trader --paper --strategy=momentum

# Auto strategy selection (regime-based)
./trader --paper --auto-strategy

# Composite (multiple strategies vote)
./trader --paper --composite

# Production
./trader --strategy=technical-indicators
./trader --strategy=market-maker

# List available strategies
./trader --list-strategies
```

---

## Özet

| Katman | Sorumluluk | Paper/Prod Farkı |
|--------|-----------|------------------|
| Strategy | Sinyal üret | Yok |
| Selector | Strategy seç | Yok |
| Execution | Order gönder | Yok |
| Exchange | Fill simule/gerçek | Slippage sim sadece paper |

Bu yapı ile:
- Tüm stratejiler kullanılabilir
- Regime-based akıllı seçim çalışır
- Limit/Market order dinamik seçilir
- Paper vs Prod sadece exchange layer'da ayrılır
