#pragma once
#include "../strategy/istrategy.hpp"
#include "../strategy/metrics_context.hpp"
#include "order_request.hpp"

#include <string_view>
#include <vector>

namespace hft {
namespace execution {

/// Context passed to all pipeline stages
struct ExecutionContext {
    Symbol symbol = 0;
    strategy::MarketSnapshot market;
    strategy::StrategyPosition position;
    strategy::MarketRegime regime = strategy::MarketRegime::Ranging;
    const strategy::MetricsContext* metrics = nullptr;
};

/// Base interface for execution pipeline stages
class IExecutionStage {
public:
    virtual ~IExecutionStage() = default; // LCOV_EXCL_LINE

    /// Process signal, return zero or more OrderRequests
    /// Each stage independently decides whether to produce orders
    virtual std::vector<OrderRequest> process(const strategy::Signal& signal, const ExecutionContext& ctx) = 0;

    /// Stage name for logging
    virtual std::string_view name() const = 0;

    /// Is this stage enabled? (can be toggled at runtime)
    virtual bool enabled() const { return true; }
};

} // namespace execution
} // namespace hft
