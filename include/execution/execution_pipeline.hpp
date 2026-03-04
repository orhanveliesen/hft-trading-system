#pragma once
#include "execution_stage.hpp"

#include <memory>
#include <vector>

namespace hft {
namespace execution {

/// Runs signal through all registered stages, collects OrderRequests
class ExecutionPipeline {
public:
    void add_stage(std::unique_ptr<IExecutionStage> stage) { stages_.push_back(std::move(stage)); }

    /// Process signal through all enabled stages
    /// Returns combined OrderRequests from all stages
    std::vector<OrderRequest> process(const strategy::Signal& signal, const ExecutionContext& ctx) const {
        std::vector<OrderRequest> all_requests;
        all_requests.reserve(stages_.size()); // Typical: 1 request per stage

        for (const auto& stage : stages_) {
            if (!stage->enabled())
                continue;

            auto requests = stage->process(signal, ctx);
            for (auto& req : requests) {
                if (req.is_valid()) {
                    all_requests.push_back(std::move(req));
                }
            }
        }

        return all_requests;
    }

    size_t stage_count() const { return stages_.size(); }
    bool empty() const { return stages_.empty(); }

    /// Get stage names (for logging/debug)
    std::vector<std::string_view> stage_names() const {
        std::vector<std::string_view> names;
        names.reserve(stages_.size());
        for (const auto& s : stages_) {
            names.push_back(s->name());
        }
        return names;
    }

private:
    std::vector<std::unique_ptr<IExecutionStage>> stages_;
};

} // namespace execution
} // namespace hft
