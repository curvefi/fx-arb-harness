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
    arb::events::CandleTape tape;
    const bool block = opts.event_mode == "block";
    if (block != !opts.block_tape_path.empty())
        throw std::invalid_argument("block_tape_path requires event_mode='block' and vice versa");
    if (block) {
        if (!market_path.empty() || opts.candle_filter_pct != 0 || opts.max_candles)
            throw std::invalid_argument("block requires block_tape_path and no market_path, candle_filter or n_candles");
    } else if (opts.event_mode == "candles") {
        if (market_path.empty())
            throw std::invalid_argument("candles requires market_path");
        const double filter_squeeze = opts.candle_filter_pct > 0.0
            ? opts.candle_filter_pct / 100.0 : 0.999;
        const auto source = arb::load_candles(market_path, opts.max_candles, filter_squeeze, opts.start_ts);
        if (source.empty()) throw std::runtime_error("No candles loaded for scenario '" + scenario_id + "'");
        tape = arb::events::make_candle_tape(source);
    } else {
        throw std::invalid_argument("event_mode must be 'candles' or 'block'");
    }
    std::vector<arb::Candle> candles;
    std::vector<arb::price_feeds::PriceFeedPoint> feed_points;
    if (!price_feed_path.empty()) {
        feed_points = arb::price_feeds::load_price_feed_csv(price_feed_path);
        feed_points.erase(std::remove_if(feed_points.begin(), feed_points.end(), [&](const auto& point) {
            return excluded(point.ts);
        }), feed_points.end());
    }
    if (block) {
        scenario.events = arb::events::gen_block_events(
            arb::events::load_block_tape(opts.block_tape_path), opts.arb_settle_offset_s,
            candles, opts.start_ts, opts.end_ts, opts.excluded_time_ranges);
        if (scenario.events.empty()) throw std::invalid_argument("No events in the requested time range");
    } else {
        auto events = arb::events::gen_candle_events(
            tape, candles, opts.start_ts, opts.end_ts, opts.excluded_time_ranges);
        if (events.empty()) throw std::invalid_argument("No events in the requested time range");
        if (!feed_points.empty()) arb::price_feeds::attach_price_feed(events, feed_points);
        scenario.events = arb::EventSoA::from_events(events);
    }

    scenario.id = scenario_id;
    scenario.candles = std::move(candles);
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

    scenario_ = std::move(scenario);
}

// Explicit template instantiation
template class ScenarioStore<RealT>;

} // namespace curve_fx::evaluator
