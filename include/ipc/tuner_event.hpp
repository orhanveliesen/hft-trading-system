#pragma once

/**
 * Tuner Event Types
 *
 * All trackable events in the HFT + AI Tuner system.
 * Used for audit logging, web dashboard, and analytics.
 *
 * Binary-compatible struct for fast IPC via shared ring buffer.
 */

#include <chrono>
#include <cstdint>
#include <cstring>
#include <unistd.h>

namespace hft {
namespace ipc {

// Event structure constants
constexpr size_t EVENT_REASON_LEN = 128;
constexpr size_t EVENT_SYMBOL_LEN = 16;
constexpr size_t EVENT_PARAM_NAME_LEN = 24;

/**
 * Event type categories
 */
enum class TunerEventType : uint8_t {
    // Trade events (0-15)
    Signal = 0,               // Strategy generated signal
    Order = 1,                // Order placed
    Fill = 2,                 // Order executed
    Cancel = 3,               // Order cancelled
    PositionOpen = 4,         // New position opened
    PositionClose = 5,        // Position closed
    AccumulationDecision = 6, // Trader decided on accumulation aggressiveness

    // Tuner events (16-31)
    ConfigChange = 16,  // AI modified symbol config
    PauseSymbol = 17,   // Trading paused for symbol
    ResumeSymbol = 18,  // Trading resumed for symbol
    EmergencyExit = 19, // Emergency position close
    AIDecision = 20,    // Raw AI response logged
    TuningTrigger = 21, // What triggered the tuning
    TuningSkipped = 22, // Tuning skipped (rate limit, etc)

    // Market events (32-47)
    RegimeChange = 32,    // Market regime transition
    NewsEvent = 33,       // External news detected
    VolatilitySpike = 34, // Unusual volatility
    PriceAlert = 35,      // Price threshold crossed

    // System events (48-63)
    Heartbeat = 48,    // Process health
    ProcessStart = 49, // Process started
    ProcessStop = 50,  // Process stopped
    Error = 51,        // Error or warning
    ConfigReload = 52  // Config reloaded from file
};

/**
 * What triggered a tuning request
 */
enum class TriggerReason : uint8_t {
    None = 0,
    Scheduled = 1,         // Regular interval (every N minutes)
    LossThreshold = 2,     // Symbol hit loss limit
    ConsecutiveLosses = 3, // Multiple losing trades in row
    WinStreak = 4,         // Good performance (may reduce risk)
    VolatilitySpike = 5,   // ATR increased significantly
    NewsTriggered = 6,     // Breaking news detected
    ManualRequest = 7,     // User requested via control
    StartupInit = 8,       // Initial config on startup
    RegimeChange = 9,      // Market regime changed
    DrawdownAlert = 10     // Total portfolio drawdown
};

/**
 * Event severity levels
 */
enum class Severity : uint8_t {
    Debug = 0,   // Verbose debugging info
    Info = 1,    // Normal operation
    Warning = 2, // Potential issue
    Critical = 3 // Requires attention
};

/**
 * Trade side for trade events
 */
enum class TradeSide : int8_t { Sell = -1, None = 0, Buy = 1 };

/**
 * Main event structure
 * Packed for binary IPC, 256 bytes total
 */
#pragma pack(push, 1)
struct TunerEvent {
    // === Header (16 bytes) ===
    uint64_t timestamp_ns;   // Nanoseconds since epoch
    uint32_t sequence;       // Global sequence number
    TunerEventType type;     // Event type
    TriggerReason trigger;   // Trigger reason (for tuner events)
    Severity severity;       // Event severity
    uint8_t reserved_header; // Padding

    // === Identity (20 bytes) ===
    char symbol[EVENT_SYMBOL_LEN]; // Symbol or "*" for global
    uint32_t process_id;           // Originating process

    // === Payload (92 bytes) ===
    union {
        // For Signal/Order/Fill/PositionOpen/PositionClose
        struct {
            TradeSide side;     // Buy or sell
            uint8_t order_type; // 0=market, 1=limit
            uint8_t fill_type;  // 0=full, 1=partial
            uint8_t reserved;
            double price;          // Price
            double quantity;       // Quantity
            double avg_price;      // Average fill price
            int64_t pnl_x100;      // P&L in cents (for fills)
            int64_t position_x100; // Position size after trade
            uint64_t order_id;     // Order ID
            uint64_t latency_ns;   // Order->fill latency
        } trade;                   // 60 bytes

        // For ConfigChange
        struct {
            char param_name[EVENT_PARAM_NAME_LEN]; // Parameter changed
            int32_t old_value_x100;                // Old value (scaled)
            int32_t new_value_x100;                // New value (scaled)
            uint8_t ai_confidence;                 // AI confidence 0-100
            uint8_t ai_urgency;                    // 0=low, 1=medium, 2=high
            uint8_t change_source;                 // 0=ai, 1=manual, 2=rule
            uint8_t reserved[5];
        } config; // 40 bytes

        // For RegimeChange
        struct {
            uint8_t old_regime; // Previous regime
            uint8_t new_regime; // New regime
            uint8_t reserved[2];
            double old_confidence; // Previous confidence
            double new_confidence; // New confidence
            double volatility;     // Current volatility
            double trend_strength; // Trend indicator
        } regime;                  // 36 bytes

        // For AIDecision
        struct {
            uint8_t confidence;          // Overall confidence
            uint8_t urgency;             // 0-2
            uint8_t action_taken;        // TunerCommand::Action value
            uint8_t symbols_affected;    // Count of symbols changed
            uint32_t latency_ms;         // API call latency
            uint32_t tokens_input;       // Input tokens used
            uint32_t tokens_output;      // Output tokens used
            int64_t estimated_cost_x100; // Cost in cents
        } ai;                            // 24 bytes

        // For NewsEvent
        struct {
            uint8_t sentiment; // 0=negative, 1=neutral, 2=positive
            uint8_t relevance; // 0-100
            uint8_t reserved[2];
            uint64_t news_id; // External news ID
            char source[16];  // News source name
        } news;               // 28 bytes

        // For Error
        struct {
            int32_t error_code;     // Error code
            uint8_t is_recoverable; // Can system recover?
            uint8_t reserved[3];
            char component[24]; // Which component errored
        } error;                // 32 bytes

        // For AccumulationDecision
        struct {
            double position_pct_before; // Position % before decision
            double signal_strength;     // Raw signal before reduction
            int8_t factor_x100;         // Accumulation factor used (20-80)
            int8_t regime;              // Market regime at decision
            int8_t consecutive_wins;    // Win streak
            int8_t consecutive_losses;  // Loss streak
            uint8_t reserved[4];
        } accumulation; // 24 bytes

        // Raw bytes for custom usage
        uint8_t raw[92];
    } payload;

    // === Reason/Description (128 bytes) ===
    char reason[EVENT_REASON_LEN];

    // === Helper Methods ===

    void init(TunerEventType t, const char* sym = nullptr) {
        std::memset(this, 0, sizeof(*this));
        timestamp_ns = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        type = t;
        severity = Severity::Info;
        process_id = static_cast<uint32_t>(getpid());
        if (sym) {
            std::strncpy(symbol, sym, EVENT_SYMBOL_LEN - 1);
        }
    }

    void set_reason(const char* r) {
        std::strncpy(reason, r, EVENT_REASON_LEN - 1);
        reason[EVENT_REASON_LEN - 1] = '\0';
    }

    // Trade event helpers
    static TunerEvent make_signal(const char* sym, TradeSide side, double price, double qty, const char* r = nullptr) {
        TunerEvent e;
        e.init(TunerEventType::Signal, sym);
        e.payload.trade.side = side;
        e.payload.trade.price = price;
        e.payload.trade.quantity = qty;
        if (r)
            e.set_reason(r);
        return e;
    }

    static TunerEvent make_fill(const char* sym, TradeSide side, double price, double qty, int64_t pnl_cents,
                                const char* r = nullptr) {
        TunerEvent e;
        e.init(TunerEventType::Fill, sym);
        e.payload.trade.side = side;
        e.payload.trade.price = price;
        e.payload.trade.quantity = qty;
        e.payload.trade.pnl_x100 = pnl_cents;
        if (r)
            e.set_reason(r);
        return e;
    }

    static TunerEvent make_config_change(const char* sym, const char* param, int32_t old_val, int32_t new_val,
                                         uint8_t confidence, const char* r = nullptr) {
        TunerEvent e;
        e.init(TunerEventType::ConfigChange, sym);
        std::strncpy(e.payload.config.param_name, param, EVENT_PARAM_NAME_LEN - 1);
        e.payload.config.old_value_x100 = old_val;
        e.payload.config.new_value_x100 = new_val;
        e.payload.config.ai_confidence = confidence;
        if (r)
            e.set_reason(r);
        return e;
    }

    static TunerEvent make_regime_change(const char* sym, uint8_t old_r, uint8_t new_r, double confidence,
                                         const char* r = nullptr) {
        TunerEvent e;
        e.init(TunerEventType::RegimeChange, sym);
        e.payload.regime.old_regime = old_r;
        e.payload.regime.new_regime = new_r;
        e.payload.regime.new_confidence = confidence;
        if (r)
            e.set_reason(r);
        return e;
    }

    static TunerEvent make_ai_decision(uint8_t confidence, uint8_t urgency, uint8_t action, uint32_t latency,
                                       const char* r = nullptr) {
        TunerEvent e;
        e.init(TunerEventType::AIDecision, "*");
        e.payload.ai.confidence = confidence;
        e.payload.ai.urgency = urgency;
        e.payload.ai.action_taken = action;
        e.payload.ai.latency_ms = latency;
        if (r)
            e.set_reason(r);
        return e;
    }

    static TunerEvent make_error(const char* component, int32_t code, bool recoverable, const char* r) {
        TunerEvent e;
        e.init(TunerEventType::Error, "*");
        e.severity = Severity::Critical;
        e.payload.error.error_code = code;
        e.payload.error.is_recoverable = recoverable ? 1 : 0;
        std::strncpy(e.payload.error.component, component, 23);
        e.set_reason(r);
        return e;
    }

    static TunerEvent make_accumulation(const char* sym, double pos_pct, double signal_strength, int factor_x100,
                                        uint8_t regime, int8_t wins, int8_t losses, const char* r = nullptr) {
        TunerEvent e;
        e.init(TunerEventType::AccumulationDecision, sym);
        e.payload.accumulation.position_pct_before = pos_pct;
        e.payload.accumulation.signal_strength = signal_strength;
        e.payload.accumulation.factor_x100 = static_cast<int8_t>(factor_x100);
        e.payload.accumulation.regime = static_cast<int8_t>(regime);
        e.payload.accumulation.consecutive_wins = wins;
        e.payload.accumulation.consecutive_losses = losses;
        if (r)
            e.set_reason(r);
        return e;
    }

    // Type checking helpers
    bool is_trade_event() const { return static_cast<uint8_t>(type) < 16; }

    bool is_tuner_event() const {
        auto t = static_cast<uint8_t>(type);
        return t >= 16 && t < 32;
    }

    bool is_market_event() const {
        auto t = static_cast<uint8_t>(type);
        return t >= 32 && t < 48;
    }

    bool is_system_event() const { return static_cast<uint8_t>(type) >= 48; }

    // Get type name
    const char* type_name() const {
        switch (type) {
        case TunerEventType::Signal:
            return "SIGNAL";
        case TunerEventType::Order:
            return "ORDER";
        case TunerEventType::Fill:
            return "FILL";
        case TunerEventType::Cancel:
            return "CANCEL";
        case TunerEventType::PositionOpen:
            return "POS_OPEN";
        case TunerEventType::PositionClose:
            return "POS_CLOSE";
        case TunerEventType::AccumulationDecision:
            return "ACCUMULATION";
        case TunerEventType::ConfigChange:
            return "CONFIG";
        case TunerEventType::PauseSymbol:
            return "PAUSE";
        case TunerEventType::ResumeSymbol:
            return "RESUME";
        case TunerEventType::EmergencyExit:
            return "EMERGENCY";
        case TunerEventType::AIDecision:
            return "AI_DECISION";
        case TunerEventType::TuningTrigger:
            return "TRIGGER";
        case TunerEventType::TuningSkipped:
            return "SKIP";
        case TunerEventType::RegimeChange:
            return "REGIME";
        case TunerEventType::NewsEvent:
            return "NEWS";
        case TunerEventType::VolatilitySpike:
            return "VOL_SPIKE";
        case TunerEventType::PriceAlert:
            return "PRICE_ALERT";
        case TunerEventType::Heartbeat:
            return "HEARTBEAT";
        case TunerEventType::ProcessStart:
            return "START";
        case TunerEventType::ProcessStop:
            return "STOP";
        case TunerEventType::Error:
            return "ERROR";
        case TunerEventType::ConfigReload:
            return "RELOAD";
        default:
            return "UNKNOWN";
        }
    }

    // Get trigger name
    const char* trigger_name() const {
        switch (trigger) {
        case TriggerReason::None:
            return "";
        case TriggerReason::Scheduled:
            return "scheduled";
        case TriggerReason::LossThreshold:
            return "loss_threshold";
        case TriggerReason::ConsecutiveLosses:
            return "consecutive_losses";
        case TriggerReason::WinStreak:
            return "win_streak";
        case TriggerReason::VolatilitySpike:
            return "volatility_spike";
        case TriggerReason::NewsTriggered:
            return "news";
        case TriggerReason::ManualRequest:
            return "manual";
        case TriggerReason::StartupInit:
            return "startup";
        case TriggerReason::RegimeChange:
            return "regime_change";
        case TriggerReason::DrawdownAlert:
            return "drawdown";
        default:
            return "unknown";
        }
    }
};
#pragma pack(pop)

static_assert(sizeof(TunerEvent) == 256, "TunerEvent must be 256 bytes");

} // namespace ipc
} // namespace hft
