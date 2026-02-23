/**
 * Test ClaudeClient JSON Parser - Tuner Command Parsing
 *
 * Tests that ALL SymbolTuningConfig parameters can be parsed from Claude's JSON response.
 * This ensures the tuner can fully control trading behavior.
 */

#include "../include/tuner/claude_client.hpp"

#include <cassert>
#include <cstring>
#include <iostream>

#define TEST(name) void name()

#define RUN_TEST(name)                                                                                                 \
    do {                                                                                                               \
        std::cout << "  " << #name << "... ";                                                                          \
        try {                                                                                                          \
            name();                                                                                                    \
            std::cout << "PASSED\n";                                                                                   \
        } catch (...) {                                                                                                \
            std::cout << "FAILED (exception)\n";                                                                       \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

#define ASSERT_EQ(a, b)                                                                                                \
    do {                                                                                                               \
        if ((a) != (b)) {                                                                                              \
            std::cerr << "\nFAIL: " << #a << " (" << (a) << ") != " << #b << " (" << (b) << ")\n";                     \
            assert(false);                                                                                             \
        }                                                                                                              \
    } while (0)

#define ASSERT_TRUE(expr)                                                                                              \
    do {                                                                                                               \
        if (!(expr)) {                                                                                                 \
            std::cerr << "\nFAIL: " << #expr << " is false\n";                                                         \
            assert(false);                                                                                             \
        }                                                                                                              \
    } while (0)

#define ASSERT_NEAR(a, b, eps)                                                                                         \
    do {                                                                                                               \
        double _a = (a), _b = (b), _eps = (eps);                                                                       \
        if (std::abs(_a - _b) > _eps) {                                                                                \
            std::cerr << "\nFAIL: " << #a << " (" << _a << ") != " << #b << " (" << _b << ") within " << _eps << "\n"; \
            assert(false);                                                                                             \
        }                                                                                                              \
    } while (0)

/**
 * Test wrapper that exposes private parsing methods for testing
 */
class TestableClaudeClient : public hft::tuner::ClaudeClient {
public:
    // Expose private parsing methods for testing
    bool test_parse_tuner_command(const std::string& text, hft::ipc::TunerCommand& cmd) {
        return parse_tuner_command(text, cmd);
    }

    std::string test_extract_string(const std::string& json, const char* key) { return extract_string(json, key); }

    double test_extract_number(const std::string& json, const char* key) { return extract_number(json, key); }
};

// =============================================================================
// TEST 1: Parse basic UPDATE_CONFIG action
// =============================================================================
TEST(parse_basic_update_config) {
    TestableClaudeClient client;
    hft::ipc::TunerCommand cmd;

    std::string json = R"({
        "action": "UPDATE_CONFIG",
        "symbol": "BTCUSDT",
        "confidence": 85,
        "urgency": 1,
        "reason": "High win rate, increasing position",
        "config": {
            "ema_dev_trending_pct": 1.5,
            "base_position_pct": 5.0,
            "target_pct": 3.0,
            "stop_pct": 4.0
        }
    })";

    bool parsed = client.test_parse_tuner_command(json, cmd);
    ASSERT_TRUE(parsed);
    ASSERT_TRUE(cmd.action == hft::ipc::TunerCommand::Action::UpdateSymbolConfig);
    ASSERT_EQ(std::string(cmd.symbol), "BTCUSDT");
    ASSERT_EQ(cmd.confidence, 85);
    ASSERT_EQ(cmd.urgency, 1);

    // Verify config values
    ASSERT_EQ(cmd.config.ema_dev_trending_x100, 150); // 1.5% * 100
    ASSERT_EQ(cmd.config.base_position_x100, 500);    // 5.0% * 100
    ASSERT_EQ(cmd.config.target_pct_x100, 300);       // 3.0% * 100
    ASSERT_EQ(cmd.config.stop_pct_x100, 400);         // 4.0% * 100
}

// =============================================================================
// TEST 2: Parse mode threshold parameters (NEW)
// =============================================================================
TEST(parse_mode_thresholds) {
    TestableClaudeClient client;
    hft::ipc::TunerCommand cmd;

    std::string json = R"({
        "action": "UPDATE_CONFIG",
        "symbol": "BTCUSDT",
        "confidence": 80,
        "reason": "Adjusting mode thresholds",
        "config": {
            "losses_to_cautious": 3,
            "losses_to_defensive": 5,
            "losses_to_exit_only": 7,
            "wins_to_aggressive": 4
        }
    })";

    bool parsed = client.test_parse_tuner_command(json, cmd);
    ASSERT_TRUE(parsed);
    ASSERT_EQ(cmd.config.losses_to_cautious, 3);
    ASSERT_EQ(cmd.config.losses_to_defensive, 5);
    ASSERT_EQ(cmd.config.losses_to_exit_only, 7);
    ASSERT_EQ(cmd.config.wins_to_aggressive, 4);
}

// =============================================================================
// TEST 3: Parse signal threshold parameters (NEW)
// =============================================================================
TEST(parse_signal_thresholds) {
    TestableClaudeClient client;
    hft::ipc::TunerCommand cmd;

    std::string json = R"({
        "action": "UPDATE_CONFIG",
        "symbol": "ETHUSDT",
        "confidence": 75,
        "reason": "Adjusting signal thresholds",
        "config": {
            "signal_aggressive": 0.25,
            "signal_normal": 0.45,
            "signal_cautious": 0.65,
            "min_confidence": 0.35
        }
    })";

    bool parsed = client.test_parse_tuner_command(json, cmd);
    ASSERT_TRUE(parsed);
    ASSERT_EQ(cmd.config.signal_aggressive_x100, 25); // 0.25 * 100
    ASSERT_EQ(cmd.config.signal_normal_x100, 45);     // 0.45 * 100
    ASSERT_EQ(cmd.config.signal_cautious_x100, 65);   // 0.65 * 100
    ASSERT_EQ(cmd.config.min_confidence_x100, 35);    // 0.35 * 100
}

// =============================================================================
// TEST 4: Parse accumulation control parameters (NEW)
// =============================================================================
TEST(parse_accumulation_control) {
    TestableClaudeClient client;
    hft::ipc::TunerCommand cmd;

    std::string json = R"({
        "action": "UPDATE_CONFIG",
        "symbol": "SOLUSDT",
        "confidence": 70,
        "reason": "Tuning accumulation behavior",
        "config": {
            "accum_floor_trending": 0.55,
            "accum_floor_ranging": 0.35,
            "accum_floor_highvol": 0.25,
            "accum_boost_win": 0.15,
            "accum_penalty_loss": 0.12,
            "accum_max": 0.85
        }
    })";

    bool parsed = client.test_parse_tuner_command(json, cmd);
    ASSERT_TRUE(parsed);
    ASSERT_EQ(cmd.config.accum_floor_trending_x100, 55);   // 0.55 * 100
    ASSERT_EQ(cmd.config.accum_floor_ranging_x100, 35);    // 0.35 * 100
    ASSERT_EQ(cmd.config.accum_floor_highvol_x100, 25);    // 0.25 * 100
    ASSERT_EQ(cmd.config.accum_boost_per_win_x100, 15);    // 0.15 * 100
    ASSERT_EQ(cmd.config.accum_penalty_per_loss_x100, 12); // 0.12 * 100
    ASSERT_EQ(cmd.config.accum_max_x100, 85);              // 0.85 * 100
}

// =============================================================================
// TEST 5: Parse minimum position parameter (NEW)
// =============================================================================
TEST(parse_min_position) {
    TestableClaudeClient client;
    hft::ipc::TunerCommand cmd;

    std::string json = R"({
        "action": "UPDATE_CONFIG",
        "symbol": "BTCUSDT",
        "confidence": 80,
        "reason": "Adjusting position limits",
        "config": {
            "base_position_pct": 3.0,
            "max_position_pct": 10.0,
            "min_position_pct": 0.5
        }
    })";

    bool parsed = client.test_parse_tuner_command(json, cmd);
    ASSERT_TRUE(parsed);
    ASSERT_EQ(cmd.config.base_position_x100, 300); // 3.0% * 100
    ASSERT_EQ(cmd.config.max_position_x100, 1000); // 10.0% * 100
    ASSERT_EQ(cmd.config.min_position_x100, 50);   // 0.5% * 100
}

// =============================================================================
// TEST 6: Parse ALL config parameters in single response
// =============================================================================
TEST(parse_all_config_parameters) {
    TestableClaudeClient client;
    hft::ipc::TunerCommand cmd;

    std::string json = R"({
        "action": "UPDATE_CONFIG",
        "symbol": "BTCUSDT",
        "confidence": 90,
        "urgency": 2,
        "reason": "Full parameter update",
        "config": {
            "ema_dev_trending_pct": 1.2,
            "ema_dev_ranging_pct": 0.6,
            "ema_dev_highvol_pct": 0.3,
            "base_position_pct": 4.0,
            "max_position_pct": 12.0,
            "min_position_pct": 0.8,
            "cooldown_ms": 3000,
            "signal_strength": 2,
            "target_pct": 3.5,
            "stop_pct": 4.5,
            "pullback_pct": 0.8,
            "order_type": "Adaptive",
            "limit_offset_bps": 3.0,
            "limit_timeout_ms": 800,
            "losses_to_cautious": 2,
            "losses_to_defensive": 4,
            "losses_to_exit_only": 6,
            "wins_to_aggressive": 3,
            "signal_aggressive": 0.30,
            "signal_normal": 0.50,
            "signal_cautious": 0.70,
            "min_confidence": 0.30,
            "accum_floor_trending": 0.50,
            "accum_floor_ranging": 0.30,
            "accum_floor_highvol": 0.20,
            "accum_boost_win": 0.10,
            "accum_penalty_loss": 0.10,
            "accum_max": 0.80
        }
    })";

    bool parsed = client.test_parse_tuner_command(json, cmd);
    ASSERT_TRUE(parsed);

    // Basic config
    ASSERT_EQ(cmd.config.ema_dev_trending_x100, 120);
    ASSERT_EQ(cmd.config.ema_dev_ranging_x100, 60);
    ASSERT_EQ(cmd.config.ema_dev_highvol_x100, 30);
    ASSERT_EQ(cmd.config.base_position_x100, 400);
    ASSERT_EQ(cmd.config.max_position_x100, 1200);
    ASSERT_EQ(cmd.config.min_position_x100, 80);
    ASSERT_EQ(cmd.config.cooldown_ms, 3000);
    ASSERT_EQ(cmd.config.signal_strength, 2);
    ASSERT_EQ(cmd.config.target_pct_x100, 350);
    ASSERT_EQ(cmd.config.stop_pct_x100, 450);
    ASSERT_EQ(cmd.config.pullback_pct_x100, 80);
    ASSERT_EQ(cmd.config.order_type_preference, 3); // Adaptive
    ASSERT_EQ(cmd.config.limit_offset_bps_x100, 300);
    ASSERT_EQ(cmd.config.limit_timeout_ms, 800);

    // Mode thresholds
    ASSERT_EQ(cmd.config.losses_to_cautious, 2);
    ASSERT_EQ(cmd.config.losses_to_defensive, 4);
    ASSERT_EQ(cmd.config.losses_to_exit_only, 6);
    ASSERT_EQ(cmd.config.wins_to_aggressive, 3);

    // Signal thresholds
    ASSERT_EQ(cmd.config.signal_aggressive_x100, 30);
    ASSERT_EQ(cmd.config.signal_normal_x100, 50);
    ASSERT_EQ(cmd.config.signal_cautious_x100, 70);
    ASSERT_EQ(cmd.config.min_confidence_x100, 30);

    // Accumulation
    ASSERT_EQ(cmd.config.accum_floor_trending_x100, 50);
    ASSERT_EQ(cmd.config.accum_floor_ranging_x100, 30);
    ASSERT_EQ(cmd.config.accum_floor_highvol_x100, 20);
    ASSERT_EQ(cmd.config.accum_boost_per_win_x100, 10);
    ASSERT_EQ(cmd.config.accum_penalty_per_loss_x100, 10);
    ASSERT_EQ(cmd.config.accum_max_x100, 80);
}

// =============================================================================
// TEST 7: Parse with markdown code blocks (Claude often wraps in ```)
// =============================================================================
TEST(parse_with_markdown_blocks) {
    TestableClaudeClient client;
    hft::ipc::TunerCommand cmd;

    std::string response = R"(
Based on the analysis, I recommend:

```json
{
    "action": "UPDATE_CONFIG",
    "symbol": "BTCUSDT",
    "confidence": 75,
    "reason": "Testing markdown",
    "config": {
        "losses_to_cautious": 3,
        "accum_floor_trending": 0.60
    }
}
```

This should improve performance.
)";

    bool parsed = client.test_parse_tuner_command(response, cmd);
    ASSERT_TRUE(parsed);
    ASSERT_TRUE(cmd.action == hft::ipc::TunerCommand::Action::UpdateSymbolConfig);
    ASSERT_EQ(cmd.config.losses_to_cautious, 3);
    ASSERT_EQ(cmd.config.accum_floor_trending_x100, 60);
}

// =============================================================================
// MAIN
// =============================================================================
int main() {
    std::cout << "Running ClaudeClient Parser Tests:\n";

    RUN_TEST(parse_basic_update_config);
    RUN_TEST(parse_mode_thresholds);
    RUN_TEST(parse_signal_thresholds);
    RUN_TEST(parse_accumulation_control);
    RUN_TEST(parse_min_position);
    RUN_TEST(parse_all_config_parameters);
    RUN_TEST(parse_with_markdown_blocks);

    std::cout << "\nAll tests passed!\n";
    return 0;
}
