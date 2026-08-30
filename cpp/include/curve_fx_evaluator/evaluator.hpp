#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <boost/json.hpp>

#include "core/common.hpp"
#include "core/json_utils.hpp"
#include "curve_fx_evaluator/trace.hpp"
#include "curve_fx_evaluator/types.hpp"
#include "events/loader.hpp"
#include "events/types.hpp"
#include "harness/actions.hpp"
#include "harness/detailed_output.hpp"
#include "harness/precision.hpp"
#include "harness/run_config.hpp"
#include "harness/runner.hpp"
#include "oracles/chainlink.hpp"
#include "pools/pool_config_source.hpp"
#include "pools/pool_config_parse.hpp"
#include "pools/pool_init.hpp"
#include "trading/costs.hpp"

namespace curve_fx::evaluator {

struct ScenarioLoadOptions {
    size_t pool_index{0};
    size_t max_candles{0};
    uint64_t start_ts{0};
    uint64_t end_ts{0};
    double candle_filter_pct{0.0};
};

template <typename T = RealT>
struct Scenario {
    std::string id;
    std::string candle_path;
    std::string chainlink_path;
    std::vector<arb::Candle> candles;
    arb::EventSoA events;
    arb::pools::PoolInit<T> base_pool;
    arb::trading::Costs<T> base_costs;
    uint64_t start_ts{0};
};

template <typename T = RealT>
struct SessionConfig {
    T min_swap_frac{static_cast<T>(1e-6)};
    T max_swap_frac{static_cast<T>(1.0)};
    uint64_t start_ts{0};
    uint64_t dustswap_freq_s{3600};
    bool dustswap_random{false};
    uint64_t dustswap_dynamic_freq_s{0};
    bool dustswap_dynamic_gap_enabled{false};
    T dustswap_dynamic_gap_bps{static_cast<T>(0)};
    uint64_t dustswap_dynamic_heartbeat_s{0};
    uint64_t dustswap_commit_clock_freq_s{0};
    bool policy_keeper_enabled{false};
    bool allow_hybrid_keeper{false};
    uint64_t user_swap_freq_s{0};
    T user_swap_size_frac{static_cast<T>(0.01)};
    T user_swap_thresh{static_cast<T>(0.05)};
    bool enable_slippage_probes{false};
    std::string event_cursor{"scalar"};
    std::string metric_profile{"full_summary"};

    // YieldBasis mode: "off", "active_2l" (established Observer2-equivalent
    // lane), or "reference_2l" (contract-derived candidate lane).
    std::string yb_mode{"off"};
    T yb_releverage_fee{static_cast<T>(0.012)};

    T yb_cash_multiplier{static_cast<T>(1.0)};

};

enum class ObservationKind : uint8_t {
    Summary = 0,
    FullTrace = 1,
};

struct ObservationSpec {
    ObservationKind kind{ObservationKind::Summary};
    size_t trace_interval{1};
    bool trace_actions{false};
    std::string artifact_dir;
};

template <typename T = RealT>
struct EvaluationCandidate {
    uint32_t ordinal{0};
    std::string candidate_id;
    std::vector<T> policy_params;
    boost::json::object pool_overrides;
    std::optional<arb::pools::PoolOverride<T>> typed_pool_override;
};

struct ScenarioEvaluationResult {
    std::string scenario_id;
    bool success{false};
    std::string error_message;
    std::map<std::string, double> metrics;
    bool has_trace{false};
    std::string trace_json;
    std::string actions_json;
    uint64_t trace_record_count{0};
    uint64_t action_count{0};
};

struct CandidateEvaluationResult {
    uint32_t ordinal{0};
    std::string candidate_id;
    bool success{false};
    std::string error_message;
    std::map<std::string, double> aggregate_metrics;
    std::vector<ScenarioEvaluationResult> scenario_results;
};

struct BatchEvaluationResult {
    std::vector<CandidateEvaluationResult> candidate_results;
    double elapsed_ms{0.0};
};

void configure_worker_count(size_t count);
size_t configured_worker_count();

template <typename T = RealT>
class ScenarioStore {
public:
    ScenarioStore() = default;

    void load(
        const std::string& template_path,
        const std::string& scenario_id,
        const std::string& market_path,
        const std::string& chainlink_path,
        const ScenarioLoadOptions& opts
    );

    const std::vector<Scenario<T>>& scenarios() const {
        return scenarios_;
    }

private:
    std::vector<Scenario<T>> scenarios_;
};

// Main evaluation entry point
BatchEvaluationResult evaluate_batch_candidates(
    const ScenarioStore<RealT>& store,
    const SessionConfig<RealT>& session_cfg,
    const std::vector<EvaluationCandidate<RealT>>& candidates,
    const ObservationSpec& obs_spec
);

} // namespace curve_fx::evaluator
