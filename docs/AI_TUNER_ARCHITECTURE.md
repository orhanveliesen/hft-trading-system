# AI Tuner Architecture & Tracking System

## Overview

AI-driven per-symbol tuning system with full action tracking and web monitoring interface.

```
┌──────────────────────────────────────────────────────────────────────────┐
│                         Trader Ecosystem                                  │
├──────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  ┌─────────────┐    SharedSymbolConfigs     ┌─────────────────────┐     │
│  │   trader    │◄──────────────────────────►│   trader_tuner      │     │
│  │   (main)    │         (SHM)              │  (Claude API)       │     │
│  └──────┬──────┘                            └──────────┬──────────┘     │
│         │                                              │                 │
│         │ TradeEvents                                  │ TunerEvents    │
│         ▼                                              ▼                 │
│  ┌──────────────────────────────────────────────────────────────┐       │
│  │              SharedEventLog (Ring Buffer)                     │       │
│  │   - Trade executions       - Config changes                   │       │
│  │   - Signals generated      - AI decisions                     │       │
│  │   - Performance updates    - Emergency actions                │       │
│  └──────────────────────────────────────────────────────────────┘       │
│                              │                                           │
│                              ▼                                           │
│  ┌──────────────────────────────────────────────────────────────┐       │
│  │                  trader_web_api                               │       │
│  │   - REST endpoints        - WebSocket streaming               │       │
│  │   - SQLite persistence    - Event aggregation                 │       │
│  └──────────────────────────────────────────────────────────────┘       │
│                              │                                           │
│                              ▼                                           │
│  ┌──────────────────────────────────────────────────────────────┐       │
│  │                    Web Dashboard                              │       │
│  │   - Real-time monitoring  - Historical analysis               │       │
│  │   - Per-symbol views      - Tuner action log                  │       │
│  └──────────────────────────────────────────────────────────────┘       │
│                                                                          │
└──────────────────────────────────────────────────────────────────────────┘
```

---

## 1. Event Tracking System

### 1.1 Event Types

All trackable events in the system:

| Category | Event Type | Description |
|----------|------------|-------------|
| **Trade** | SIGNAL | Strategy generated buy/sell signal |
| **Trade** | ORDER | Order placed (paper/real) |
| **Trade** | FILL | Order executed |
| **Trade** | CANCEL | Order cancelled |
| **Tuner** | CONFIG_CHANGE | AI modified symbol config |
| **Tuner** | PAUSE_SYMBOL | AI paused trading for symbol |
| **Tuner** | RESUME_SYMBOL | AI resumed trading for symbol |
| **Tuner** | EMERGENCY_EXIT | AI triggered emergency position close |
| **Tuner** | AI_DECISION | Raw AI response (for audit) |
| **Tuner** | TRIGGER | What triggered the tuning (time/loss/event) |
| **Market** | REGIME_CHANGE | Market regime transition |
| **Market** | NEWS_EVENT | External news affecting symbol |
| **System** | HEARTBEAT | Process health check |
| **System** | ERROR | System error or warning |

### 1.2 Event Structure

```cpp
// include/ipc/tuner_event.hpp

#pragma pack(push, 1)
struct TunerEvent {
    static constexpr size_t MAX_REASON_LEN = 128;
    static constexpr size_t MAX_SYMBOL_LEN = 16;

    enum class Type : uint8_t {
        // Trade events (0-15)
        Signal = 0,
        Order = 1,
        Fill = 2,
        Cancel = 3,

        // Tuner events (16-31)
        ConfigChange = 16,
        PauseSymbol = 17,
        ResumeSymbol = 18,
        EmergencyExit = 19,
        AIDecision = 20,
        TuningTrigger = 21,

        // Market events (32-47)
        RegimeChange = 32,
        NewsEvent = 33,

        // System events (48-63)
        Heartbeat = 48,
        Error = 49
    };

    enum class TriggerReason : uint8_t {
        Scheduled = 0,      // Regular interval
        LossThreshold = 1,  // Hit loss limit
        WinStreak = 2,      // Good performance
        VolatilitySpike = 3,
        NewsTriggered = 4,
        ManualRequest = 5,
        StartupInit = 6
    };

    // Header (16 bytes)
    uint64_t timestamp_ns;      // Nanoseconds since epoch
    uint32_t sequence;          // Global sequence number
    Type type;
    TriggerReason trigger;
    uint8_t severity;           // 0=info, 1=warn, 2=critical
    uint8_t reserved;

    // Identity (20 bytes)
    char symbol[MAX_SYMBOL_LEN];
    uint32_t process_id;        // Which process generated this

    // Payload (variable by type, 64 bytes reserved)
    union {
        // For Signal/Order/Fill
        struct {
            int8_t side;        // 1=buy, -1=sell
            double price;
            double quantity;
            int64_t pnl_x100;   // For fills
        } trade;

        // For ConfigChange
        struct {
            int16_t old_value_x100;
            int16_t new_value_x100;
            char param_name[24];
        } config;

        // For RegimeChange
        struct {
            uint8_t old_regime;
            uint8_t new_regime;
            double confidence;
        } regime;

        // For AIDecision
        struct {
            uint8_t confidence;
            uint8_t urgency;
            uint16_t latency_ms;  // API call latency
        } ai;

        uint8_t raw[64];
    } payload;

    // Reason/description (128 bytes)
    char reason[MAX_REASON_LEN];
};
#pragma pack(pop)

static_assert(sizeof(TunerEvent) == 228, "TunerEvent must be 228 bytes");
```

### 1.3 Shared Event Log (Ring Buffer)

```cpp
// include/ipc/shared_event_log.hpp

struct SharedEventLog {
    static constexpr size_t RING_SIZE = 16384;  // ~3.5MB, ~16K events
    static constexpr uint64_t MAGIC = 0x4556544C4F4700ULL;  // "EVTLOG\0"

    // Header
    uint64_t magic;
    uint32_t version;
    std::atomic<uint64_t> write_pos;   // Next write position
    std::atomic<uint64_t> total_events; // Total events ever written

    // Ring buffer
    TunerEvent events[RING_SIZE];

    // Per-symbol stats (quick lookup)
    struct SymbolStats {
        char symbol[16];
        std::atomic<uint32_t> signal_count;
        std::atomic<uint32_t> fill_count;
        std::atomic<uint32_t> config_changes;
        std::atomic<int64_t> total_pnl_x100;
        std::atomic<uint64_t> last_event_ns;
    };
    SymbolStats symbol_stats[32];

    // Methods
    void log(const TunerEvent& event);
    TunerEvent* get_event(uint64_t seq);
    uint64_t get_events_since(uint64_t seq, TunerEvent* out, size_t max);
};
```

---

## 2. Tuning Triggers

### 2.1 When to Invoke Claude API

| Trigger | Condition | Priority |
|---------|-----------|----------|
| **Scheduled** | Every 5 minutes | Low |
| **Loss Threshold** | Symbol loses >2% in session | High |
| **Consecutive Losses** | 3+ losing trades in row | High |
| **Volatility Spike** | ATR increases >50% | Medium |
| **Win Streak** | 5+ winning trades | Low (reduce risk?) |
| **News Event** | Breaking news detected | High |
| **Manual** | User request via control | Immediate |

### 2.2 Data Sent to Claude API

For each tuning request, send aggregated data:

```cpp
struct TuningRequest {
    // Per-symbol data (last N minutes)
    struct SymbolData {
        char symbol[16];

        // Price data
        double current_price;
        double ema_20;
        double price_change_1m;
        double price_change_5m;
        double price_change_15m;

        // Volatility
        double atr_14;
        double atr_change_pct;

        // Our performance
        int32_t trades_last_hour;
        int32_t wins_last_hour;
        double pnl_last_hour;
        double avg_trade_duration_ms;

        // Current config
        SymbolTuningConfig current_config;

        // Market regime
        uint8_t current_regime;
        double regime_confidence;
    };

    // Global state
    double total_capital;
    double total_pnl_session;
    uint32_t active_symbols;

    // Recent events (context)
    char recent_news[512];  // Summarized news

    // Request metadata
    TriggerReason trigger;
    uint8_t urgency;
};
```

---

## 3. Web Interface Architecture

### 3.1 Components

```
┌─────────────────────────────────────────────────────────────────┐
│                    trader_web_api (C++)                         │
│                                                                 │
│  ┌──────────────────┐  ┌──────────────────┐  ┌──────────────┐  │
│  │  REST Controller │  │ WebSocket Server │  │ Event Reader │  │
│  │  (cpp-httplib)   │  │  (uWebSockets)   │  │  (SHM poll)  │  │
│  └────────┬─────────┘  └────────┬─────────┘  └──────┬───────┘  │
│           │                     │                    │          │
│           ▼                     ▼                    ▼          │
│  ┌────────────────────────────────────────────────────────┐    │
│  │                   Event Service                         │    │
│  │  - Read from SharedEventLog                             │    │
│  │  - Aggregate stats                                      │    │
│  │  - Filter by symbol/type/time                           │    │
│  └────────────────────────────────────────────────────────┘    │
│           │                                                     │
│           ▼                                                     │
│  ┌────────────────────────────────────────────────────────┐    │
│  │                 SQLite Persistence                      │    │
│  │  - Historical events (beyond ring buffer)               │    │
│  │  - Config history per symbol                            │    │
│  │  - Daily/weekly aggregates                              │    │
│  └────────────────────────────────────────────────────────┘    │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

### 3.2 REST API Endpoints

```
GET  /api/v1/status              - System status
GET  /api/v1/symbols             - List all tracked symbols
GET  /api/v1/symbols/{sym}       - Symbol detail + config
GET  /api/v1/symbols/{sym}/events?limit=100&type=fill
GET  /api/v1/events?since={seq}&limit=100
GET  /api/v1/events/stream       - WebSocket endpoint
GET  /api/v1/tuner/history       - AI decision history
GET  /api/v1/tuner/stats         - Tuning statistics
POST /api/v1/tuner/trigger       - Manual tuning request
POST /api/v1/symbols/{sym}/config - Manual config override
GET  /api/v1/performance/daily   - Daily P&L summary
GET  /api/v1/performance/symbols - Per-symbol performance
```

### 3.3 WebSocket Events

Real-time streaming via WebSocket:

```json
// Client subscribes
{"type": "subscribe", "channels": ["events", "symbols.BTCUSDT"]}

// Server streams events
{
    "type": "event",
    "data": {
        "seq": 12345,
        "timestamp": 1705123456789,
        "event_type": "fill",
        "symbol": "BTCUSDT",
        "side": "buy",
        "price": 42150.5,
        "quantity": 0.001,
        "pnl": 2.35
    }
}

// Config change notification
{
    "type": "config_change",
    "symbol": "BTCUSDT",
    "changes": {
        "ema_dev_trending": {"old": 1.0, "new": 1.5},
        "base_position": {"old": 2.0, "new": 1.5}
    },
    "reason": "High volatility detected, reducing exposure",
    "ai_confidence": 85
}
```

### 3.4 Web Frontend (Single Page App)

Technology: React + TailwindCSS (simple, no build complexity)

**Views:**

1. **Dashboard** (main)
   - Total P&L (today/week/all-time)
   - Active symbols grid with mini-charts
   - Recent events feed
   - System health indicators

2. **Symbol Detail**
   - Price chart with EMA overlay
   - Current config parameters
   - Config history timeline
   - Trade history table
   - AI decision log for this symbol

3. **Tuner Log**
   - All AI decisions chronologically
   - Filter by trigger type, symbol
   - Before/after config comparison
   - AI reasoning display

4. **Events**
   - Full event stream (filterable)
   - Export to CSV
   - Search by symbol/type/time

5. **Settings**
   - Manual config overrides
   - Trigger thresholds
   - Enable/disable symbols

---

## 4. SQLite Schema

```sql
-- Historical events (ring buffer overflow)
CREATE TABLE events (
    id INTEGER PRIMARY KEY,
    sequence INTEGER UNIQUE NOT NULL,
    timestamp_ns INTEGER NOT NULL,
    event_type INTEGER NOT NULL,
    symbol TEXT,
    payload TEXT,  -- JSON
    reason TEXT,
    created_at DATETIME DEFAULT CURRENT_TIMESTAMP
);
CREATE INDEX idx_events_symbol ON events(symbol, timestamp_ns);
CREATE INDEX idx_events_type ON events(event_type, timestamp_ns);

-- Config change history
CREATE TABLE config_history (
    id INTEGER PRIMARY KEY,
    symbol TEXT NOT NULL,
    param_name TEXT NOT NULL,
    old_value REAL,
    new_value REAL,
    trigger_reason INTEGER,
    ai_confidence INTEGER,
    ai_reasoning TEXT,
    changed_at DATETIME DEFAULT CURRENT_TIMESTAMP
);
CREATE INDEX idx_config_symbol ON config_history(symbol, changed_at);

-- Daily aggregates
CREATE TABLE daily_summary (
    date TEXT NOT NULL,
    symbol TEXT NOT NULL,
    trades INTEGER,
    wins INTEGER,
    pnl REAL,
    config_changes INTEGER,
    PRIMARY KEY (date, symbol)
);

-- AI decision audit log
CREATE TABLE ai_decisions (
    id INTEGER PRIMARY KEY,
    request_data TEXT,    -- JSON: what we sent
    response_data TEXT,   -- Raw binary (base64) or decoded JSON
    latency_ms INTEGER,
    tokens_used INTEGER,
    trigger_reason INTEGER,
    symbols_affected TEXT,
    created_at DATETIME DEFAULT CURRENT_TIMESTAMP
);
```

---

## 5. Implementation Plan

### Phase 1: Event Infrastructure
1. Create `tuner_event.hpp` with TunerEvent struct
2. Create `shared_event_log.hpp` with ring buffer
3. Add event logging to existing trader.cpp
4. Create simple CLI tool `trader_events` to dump events

### Phase 2: AI Tuner Core
1. Create `trader_tuner.cpp` skeleton
2. Implement Claude API client (libcurl + binary parsing)
3. Add tuning triggers (scheduled, loss threshold)
4. Integrate with SharedSymbolConfigs

### Phase 3: Web Backend
1. Set up cpp-httplib REST server
2. Implement core endpoints (status, symbols, events)
3. Add SQLite persistence for history
4. Add WebSocket for real-time streaming

### Phase 4: Web Frontend
1. Create React app with routing
2. Build Dashboard view
3. Build Symbol Detail view
4. Build Tuner Log view
5. Add WebSocket integration

### Phase 5: News Integration
1. Add news source fetcher (RSS/API)
2. Implement news event detection
3. Add news context to tuning requests

---

## 6. File Structure

```
trader/
├── include/ipc/
│   ├── symbol_config.hpp      ✅ Done
│   ├── tuner_event.hpp        📝 To create
│   └── shared_event_log.hpp   📝 To create
├── tools/
│   ├── trader_tuner.cpp       📝 To create
│   ├── trader_events.cpp      📝 To create (CLI event viewer)
│   └── trader_web_api.cpp     📝 To create
├── web/
│   ├── src/
│   │   ├── App.tsx
│   │   ├── pages/
│   │   │   ├── Dashboard.tsx
│   │   │   ├── SymbolDetail.tsx
│   │   │   ├── TunerLog.tsx
│   │   │   └── Events.tsx
│   │   └── components/
│   └── package.json
└── docs/
    └── AI_TUNER_ARCHITECTURE.md  ✅ This file
```

---

## 7. Dependencies

| Component | Library | Purpose |
|-----------|---------|---------|
| HTTP Client | libcurl | Claude API calls |
| REST Server | cpp-httplib | Web API (header-only) |
| WebSocket | uWebSockets | Real-time streaming |
| JSON | nlohmann/json | Serialization |
| Database | SQLite3 | Persistence |
| Frontend | React + Vite | Web UI |

---

## 8. Security Considerations

1. **API Key Storage**: Claude API key in environment variable, never in code
2. **Web API Auth**: Simple token-based auth for web interface
3. **Rate Limiting**: Max 10 tuning requests per minute to Claude
4. **Audit Trail**: All AI decisions logged with full request/response
5. **Manual Override**: Always allow human to override AI decisions
