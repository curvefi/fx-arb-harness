#include "curve_fx_evaluator/evaluator.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace curve_fx::evaluator {

template <typename T>
void ScenarioStore<T>::load(
    const std::string& template_path,
    const std::string& scenario_id,
    const std::string& market_path,
    const std::string& price_feed_path,
    const std::string& trade_flow_path,
    const ScenarioLoadOptions& opts
) {
    arb::events::validate_time_ranges(opts.excluded_time_ranges);
    const auto excluded = [&](uint64_t ts) {
        return arb::events::time_excluded(ts, opts.excluded_time_ranges);
    };
    auto pool_doc = arb::pools::PoolConfigDocument::from_file(template_path);
    if (opts.pool_index >= pool_doc.size()) {
        throw std::runtime_error("Template pool_index " + std::to_string(opts.pool_index) +
                                 " out of range (total: " + std::to_string(pool_doc.size()) + ")");
    }
    auto [base_pool, base_costs] = pool_doc.template instantiate<T>(opts.pool_index);

    Scenario<T> scenario;
    if (opts.event_mode == "trade_flow") {
        if (trade_flow_path.empty() || !market_path.empty() || opts.candle_filter_pct != 0 || opts.max_candles)
            throw std::invalid_argument("trade_flow requires trade_flow_path and no market_path, candle_filter or n_candles");
        scenario.trade_flow = arb::events::load_trade_flow(trade_flow_path);
    } else if (opts.event_mode == "candles") {
        if (market_path.empty() || !trade_flow_path.empty())
            throw std::invalid_argument("candles requires market_path and no trade_flow_path");
        const double filter_squeeze = opts.candle_filter_pct > 0.0
            ? opts.candle_filter_pct / 100.0 : 0.999;
        const auto source = arb::load_candles(market_path, opts.max_candles, filter_squeeze, opts.start_ts);
        if (source.empty()) throw std::runtime_error("No candles loaded for scenario '" + scenario_id + "'");
        scenario.trade_flow = arb::events::trade_flow_from_candles(source, opts.candle_volume);
    } else {
        throw std::invalid_argument("event_mode must be 'candles' or 'trade_flow'");
    }
    std::vector<arb::Candle> candles;
    std::vector<uint32_t> flow_bins;
    auto events = arb::events::gen_trade_flow_events(
        scenario.trade_flow, candles, flow_bins, opts.start_ts, opts.end_ts, opts.excluded_time_ranges);
    if (events.empty()) throw std::invalid_argument("No events in the requested time range");
    std::vector<arb::price_feeds::PriceFeedPoint> feed_points;
    if (!price_feed_path.empty()) {
        feed_points = arb::price_feeds::load_price_feed_csv(price_feed_path);
        feed_points.erase(std::remove_if(feed_points.begin(), feed_points.end(), [&](const auto& point) {
            return excluded(point.ts);
        }), feed_points.end());
        arb::price_feeds::attach_price_feed(events, feed_points);
    }

    scenario.id = scenario_id;
    scenario.candles = std::move(candles);
    scenario.events = arb::EventSoA::from_events(events);
    scenario.events.flow_bin = std::move(flow_bins);
    arb::events::index_trade_flow_events(scenario.events, scenario.trade_flow);
    for (const auto& report : feed_points) {
        scenario.events.report_ts.push_back(report.ts);
        scenario.events.report_prices.push_back(report.price);
    }
    // Immutable per-session indices shared by every candidate's cursor.
    if (!scenario.events.report_ts.empty()) {
        if (scenario.events.report_ts.size() > std::numeric_limits<uint32_t>::max())
            throw std::invalid_argument("Report indices exceed uint32_t");
        arb::harness::index_report_events(scenario.events);
    }
    scenario.base_pool = base_pool;
    scenario.base_costs = base_costs;
    scenario.start_ts = scenario.candles.front().ts;

    scenario_ = std::move(scenario);
}

// Explicit template instantiations
template class ScenarioStore<double>;
template class ScenarioStore<long double>;
template class ScenarioStore<float>;

} // namespace curve_fx::evaluator
