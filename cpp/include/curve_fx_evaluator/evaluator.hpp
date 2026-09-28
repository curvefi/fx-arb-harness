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
#include "events/trade_flow.hpp"
#include "events/types.hpp"
#include "harness/actions.hpp"
#include "harness/detailed_output.hpp"
#include "harness/precision.hpp"
#include "harness/run_config.hpp"
#include "harness/runner.hpp"
#include "price_feeds/price_feed.hpp"
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
    // "candles": market_path OHLCV approximates the taker flow; "trade_flow":
    // trade_flow_path holds binned trades. Both drive the maker arbitrageur.
    std::string event_mode{"candles"};
    bool candle_volume{true};  // candles only: spread volume along the path, else unlimited
    arb::events::TimeRanges excluded_time_ranges;
};

template <typename T = RealT>
struct Scenario {
    std::string id;
    std::vector<arb::Candle> candles;
    arb::EventSoA events;
    arb::events::TradeFlowTape trade_flow;
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
    uint64_t user_swap_freq_s{0};
    T user_swap_size_frac{static_cast<T>(0.01)};
    T user_swap_thresh{static_cast<T>(0.05)};
    bool enable_slippage_probes{false};
    arb::harness::EventCursor event_cursor{arb::harness::EventCursor::Scalar};
    arb::harness::MetricProfile metric_profile{arb::harness::MetricProfile::FullSummary};
    double early_stop_max_7d_rel_price_diff{0.0};

    // YieldBasis mode: "off", "active_2l" (established Observer2-equivalent
    // lane), or "reference_2l" (contract-derived candidate lane).
    arb::harness::YbMode yb_mode{arb::harness::YbMode::Off};
    T yb_releverage_fee{static_cast<T>(0.012)};
    T yb_cash_multiplier{static_cast<T>(1.0)};
    T yb_min_net_profit_coin0{static_cast<T>(1.0)};
    std::optional<arb::harness::YbInitialState<T>> yb_initial_state;

};

enum class ObservationKind : uint8_t {
    Summary = 0,
    FullTrace = 1,
};

struct ObservationSpec {
    ObservationKind kind{ObservationKind::Summary};
    size_t trace_interval{1};
    bool trace_actions{false};
};

template <typename T = RealT>
struct EvaluationCandidate {
    uint32_t ordinal{0};
    std::string candidate_id;
    std::vector<T> policy_params;
    boost::json::object pool_overrides;
    std::optional<arb::pools::PoolOverride<T>> typed_pool_override;
};

struct CandidateEvaluationResult {
    uint32_t ordinal{0};
    std::string candidate_id;
    bool success{false};
    std::string error_message;
    std::map<std::string, double> metrics;
    bool has_trace{false};
    std::string trace_json;
    std::string actions_json;
    boost::json::object effective_inputs;
    boost::json::object actor_metrics;
    uint64_t trace_record_count{0};
    uint64_t action_count{0};
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
        const std::string& price_feed_path,
        const std::string& trade_flow_path,
        const ScenarioLoadOptions& opts
    );

    const Scenario<T>& scenario() const {
        return scenario_.value();
    }

private:
    std::optional<Scenario<T>> scenario_;
};

// Main evaluation entry point
BatchEvaluationResult evaluate_batch_candidates(
    const ScenarioStore<RealT>& store,
    const SessionConfig<RealT>& session_cfg,
    const std::vector<EvaluationCandidate<RealT>>& candidates,
    const ObservationSpec& obs_spec
);

} // namespace curve_fx::evaluator
