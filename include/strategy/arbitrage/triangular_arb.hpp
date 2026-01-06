#pragma once

#include "symbol_pair.hpp"
#include "arbitrage_config.hpp"
#include "../../types.hpp"

#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <cmath>
#include <algorithm>

namespace hft {
namespace strategy {
namespace arbitrage {

/**
 * Triangular arbitrage relationship
 *
 * A triangular arbitrage involves 3 currency pairs that form a cycle:
 *   A/B → C/A → C/B
 *
 * Example: BTC/USDT → ETH/BTC → ETH/USDT
 *   - Buy BTC with USDT
 *   - Buy ETH with BTC
 *   - Sell ETH for USDT
 *   - If implied ETH/USDT > actual ETH/USDT, profit!
 */
struct TriangularRelation {
    SymbolPair leg1;  // A/B - base pair (quote is the "anchor" currency)
    SymbolPair leg2;  // C/A - cross pair
    SymbolPair leg3;  // C/B - synthetic pair

    TriangularArbConfig config;
    TriangularArbState state;

    // Symbol to leg mapping for quick updates
    int get_leg_index(const std::string& symbol) const {
        if (symbol == leg1.original || symbol == leg1.to_string()) return 1;
        if (symbol == leg2.original || symbol == leg2.to_string()) return 2;
        if (symbol == leg3.original || symbol == leg3.to_string()) return 3;
        return 0;
    }

    // Update price for a leg
    void update_price(int leg, double bid, double ask) {
        switch (leg) {
            case 1: state.leg1_bid = bid; state.leg1_ask = ask; break;
            case 2: state.leg2_bid = bid; state.leg2_ask = ask; break;
            case 3: state.leg3_bid = bid; state.leg3_ask = ask; break;
        }
    }

    /**
     * Calculate arbitrage spread
     *
     * Forward path (buy cycle):
     *   1. Buy A with B at leg1_ask
     *   2. Buy C with A at leg2_ask
     *   3. Sell C for B at leg3_bid
     *   Implied C/B = leg1_ask * leg2_ask
     *   Spread = (leg3_bid / implied) - 1
     *
     * Reverse path (sell cycle):
     *   1. Buy C with B at leg3_ask
     *   2. Sell C for A at leg2_bid
     *   3. Sell A for B at leg1_bid
     *   Implied C/B = leg1_bid * leg2_bid
     *   Spread = (implied / leg3_ask) - 1
     */
    void calculate_spreads() {
        if (!state.has_all_prices()) {
            state.forward_spread = 0.0;
            state.reverse_spread = 0.0;
            return;
        }

        // Forward: implied = leg1_ask * leg2_ask, compare to leg3_bid
        double implied_forward = state.leg1_ask * state.leg2_ask;
        if (implied_forward > 0) {
            state.forward_spread = (state.leg3_bid / implied_forward) - 1.0;
        }

        // Reverse: implied = leg1_bid * leg2_bid, compare to leg3_ask
        double implied_reverse = state.leg1_bid * state.leg2_bid;
        if (state.leg3_ask > 0) {
            state.reverse_spread = (implied_reverse / state.leg3_ask) - 1.0;
        }
    }

    // Check if there's an arbitrage opportunity
    bool has_opportunity() const {
        if (!config.enabled || !state.has_all_prices()) {
            return false;
        }
        return state.forward_spread > config.min_spread_pct ||
               state.reverse_spread > config.min_spread_pct;
    }

    // Get the profitable direction (1 = forward, -1 = reverse, 0 = none)
    int profitable_direction() const {
        if (state.forward_spread > config.min_spread_pct) return 1;
        if (state.reverse_spread > config.min_spread_pct) return -1;
        return 0;
    }

    double best_spread() const {
        return std::max(state.forward_spread, state.reverse_spread);
    }
};

/**
 * Order signal for arbitrage execution
 */
struct ArbOrderSignal {
    std::string symbol;
    Side side;
    double quantity;
    double price;  // Limit price (0 = market)
};

/**
 * Arbitrage opportunity
 */
struct ArbOpportunity {
    const TriangularRelation* relation;
    int direction;  // 1 = forward, -1 = reverse
    double spread;
    std::vector<ArbOrderSignal> orders;
    uint64_t timestamp_ns;
};

/**
 * TriangularArbDetector - Detects and monitors triangular arbitrage opportunities
 *
 * Features:
 *   - Auto-detects triangular relationships from symbol list
 *   - Maintains price state for each leg
 *   - Calculates spreads on price updates
 *   - Generates order signals when opportunities arise
 */
class TriangularArbDetector {
public:
    using OpportunityCallback = std::function<void(const ArbOpportunity&)>;

    explicit TriangularArbDetector(const ArbitrageConfig& config = ArbitrageConfig{})
        : config_(config) {}

    /**
     * Detect triangular relationships from available symbols
     *
     * @param symbols List of available trading symbols
     * @return Number of relationships detected
     */
    size_t detect_relationships(const std::vector<std::string>& symbols) {
        relations_.clear();
        symbol_to_relations_.clear();

        // Parse all symbols
        std::vector<SymbolPair> pairs;
        std::unordered_set<std::string> symbol_set;

        for (const auto& sym : symbols) {
            // Skip excluded symbols
            if (is_excluded(sym)) continue;

            auto parsed = SymbolPair::parse(sym);
            if (parsed && parsed->is_valid()) {
                pairs.push_back(*parsed);
                symbol_set.insert(parsed->to_string());
            }
        }

        // Find triangular relationships
        // For each pair A/B, find pairs C/A and C/B
        for (const auto& ab : pairs) {
            for (const auto& ca : pairs) {
                // C/A means ca.quote == ab.base
                if (ca.quote != ab.base) continue;
                if (ca.base == ab.base || ca.base == ab.quote) continue;

                // Look for C/B
                std::string cb_symbol = ca.base + "/" + ab.quote;
                if (symbol_set.find(cb_symbol) == symbol_set.end()) continue;

                // Found triangular relationship!
                TriangularRelation rel;
                rel.leg1 = ab;
                rel.leg2 = ca;
                rel.leg3 = SymbolPair{ca.base, ab.quote, cb_symbol};

                // Apply default config
                rel.config.leg1 = ab.to_string();
                rel.config.leg2 = ca.to_string();
                rel.config.leg3 = cb_symbol;
                rel.config.min_spread_pct = config_.default_min_spread_pct;
                rel.config.max_quantity = config_.default_max_quantity;
                rel.config.enabled = true;

                // Check for manual override
                apply_manual_config(rel);

                relations_.push_back(rel);

                // Limit number of relationships
                if (relations_.size() >= config_.max_auto_relationships) {
                    break;
                }
            }

            if (relations_.size() >= config_.max_auto_relationships) {
                break;
            }
        }

        // Build symbol to relations mapping for fast lookup
        build_symbol_map();

        return relations_.size();
    }

    /**
     * Update price for a symbol
     *
     * @param symbol The symbol that received new price
     * @param bid Current bid price
     * @param ask Current ask price
     * @param timestamp_ns Timestamp in nanoseconds
     * @return List of opportunities detected (if any)
     */
    std::vector<ArbOpportunity> on_price_update(
        const std::string& symbol,
        double bid,
        double ask,
        uint64_t timestamp_ns = 0
    ) {
        std::vector<ArbOpportunity> opportunities;

        auto it = symbol_to_relations_.find(symbol);
        if (it == symbol_to_relations_.end()) {
            // Try normalized format
            auto parsed = SymbolPair::parse(symbol);
            if (parsed) {
                it = symbol_to_relations_.find(parsed->to_string());
            }
        }

        if (it == symbol_to_relations_.end()) {
            return opportunities;
        }

        // Update all relations that include this symbol
        for (size_t idx : it->second) {
            auto& rel = relations_[idx];
            int leg = rel.get_leg_index(symbol);

            if (leg == 0) {
                // Try normalized
                auto parsed = SymbolPair::parse(symbol);
                if (parsed) {
                    leg = rel.get_leg_index(parsed->to_string());
                }
            }

            if (leg > 0) {
                rel.update_price(leg, bid, ask);
                rel.calculate_spreads();

                if (rel.has_opportunity()) {
                    // Check cooldown
                    if (timestamp_ns > 0 &&
                        timestamp_ns - rel.state.last_execution_ns < config_.execution_cooldown_us * 1000) {
                        continue;
                    }

                    rel.state.opportunities_detected++;

                    ArbOpportunity opp;
                    opp.relation = &rel;
                    opp.direction = rel.profitable_direction();
                    opp.spread = rel.best_spread();
                    opp.timestamp_ns = timestamp_ns;
                    opp.orders = generate_orders(rel, opp.direction);

                    opportunities.push_back(opp);

                    if (opportunity_callback_) {
                        opportunity_callback_(opp);
                    }
                }
            }
        }

        return opportunities;
    }

    /**
     * Set callback for opportunity detection
     */
    void set_opportunity_callback(OpportunityCallback callback) {
        opportunity_callback_ = std::move(callback);
    }

    /**
     * Mark an opportunity as executed
     */
    void mark_executed(const ArbOpportunity& opp, uint64_t timestamp_ns) {
        // Find and update the relation
        for (auto& rel : relations_) {
            if (&rel == opp.relation) {
                rel.state.last_execution_ns = timestamp_ns;
                rel.state.opportunities_executed++;
                rel.state.total_profit += opp.spread;
                break;
            }
        }
    }

    // Accessors
    const std::vector<TriangularRelation>& relations() const { return relations_; }
    size_t relation_count() const { return relations_.size(); }
    const ArbitrageConfig& config() const { return config_; }

    /**
     * Get all symbols involved in arbitrage relationships
     */
    std::vector<std::string> get_monitored_symbols() const {
        std::unordered_set<std::string> symbols;
        for (const auto& rel : relations_) {
            symbols.insert(rel.leg1.to_string());
            symbols.insert(rel.leg2.to_string());
            symbols.insert(rel.leg3.to_string());
        }
        return std::vector<std::string>(symbols.begin(), symbols.end());
    }

    /**
     * Get statistics summary
     */
    struct Stats {
        size_t total_relations;
        size_t active_relations;
        uint64_t total_opportunities;
        uint64_t total_executions;
        double total_profit;
    };

    Stats get_stats() const {
        Stats stats{};
        stats.total_relations = relations_.size();

        for (const auto& rel : relations_) {
            if (rel.config.enabled) stats.active_relations++;
            stats.total_opportunities += rel.state.opportunities_detected;
            stats.total_executions += rel.state.opportunities_executed;
            stats.total_profit += rel.state.total_profit;
        }

        return stats;
    }

private:
    bool is_excluded(const std::string& symbol) const {
        for (const auto& excl : config_.excluded_symbols) {
            if (symbol.find(excl) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    void apply_manual_config(TriangularRelation& rel) {
        for (const auto& manual : config_.manual_configs) {
            if ((manual.leg1 == rel.config.leg1 || manual.leg1.empty()) &&
                (manual.leg2 == rel.config.leg2 || manual.leg2.empty()) &&
                (manual.leg3 == rel.config.leg3 || manual.leg3.empty())) {
                rel.config.min_spread_pct = manual.min_spread_pct;
                rel.config.max_quantity = manual.max_quantity;
                rel.config.enabled = manual.enabled;
                break;
            }
        }
    }

    void build_symbol_map() {
        symbol_to_relations_.clear();

        for (size_t i = 0; i < relations_.size(); ++i) {
            const auto& rel = relations_[i];

            // Map by original and normalized symbols
            symbol_to_relations_[rel.leg1.original].push_back(i);
            symbol_to_relations_[rel.leg1.to_string()].push_back(i);

            symbol_to_relations_[rel.leg2.original].push_back(i);
            symbol_to_relations_[rel.leg2.to_string()].push_back(i);

            symbol_to_relations_[rel.leg3.original].push_back(i);
            symbol_to_relations_[rel.leg3.to_string()].push_back(i);
        }
    }

    std::vector<ArbOrderSignal> generate_orders(
        const TriangularRelation& rel,
        int direction
    ) const {
        std::vector<ArbOrderSignal> orders;
        double qty = rel.config.max_quantity;

        if (direction == 1) {
            // Forward: Buy A/B, Buy C/A, Sell C/B
            orders.push_back({rel.leg1.to_string(), Side::Buy, qty, rel.state.leg1_ask});
            orders.push_back({rel.leg2.to_string(), Side::Buy, qty, rel.state.leg2_ask});
            orders.push_back({rel.leg3.to_string(), Side::Sell, qty, rel.state.leg3_bid});
        } else if (direction == -1) {
            // Reverse: Buy C/B, Sell C/A, Sell A/B
            orders.push_back({rel.leg3.to_string(), Side::Buy, qty, rel.state.leg3_ask});
            orders.push_back({rel.leg2.to_string(), Side::Sell, qty, rel.state.leg2_bid});
            orders.push_back({rel.leg1.to_string(), Side::Sell, qty, rel.state.leg1_bid});
        }

        return orders;
    }

    ArbitrageConfig config_;
    std::vector<TriangularRelation> relations_;
    std::unordered_map<std::string, std::vector<size_t>> symbol_to_relations_;
    OpportunityCallback opportunity_callback_;
};

}  // namespace arbitrage
}  // namespace strategy
}  // namespace hft
