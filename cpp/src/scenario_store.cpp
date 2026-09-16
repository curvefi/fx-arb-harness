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
    const std::string& cex_depth_path,
    const ScenarioLoadOptions& opts,
    const std::string& observed_state_path
) {
    auto pool_doc = arb::pools::PoolConfigDocument::from_file(template_path);
    if (opts.pool_index >= pool_doc.size()) {
        throw std::runtime_error("Template pool_index " + std::to_string(opts.pool_index) +
                                 " out of range (total: " + std::to_string(pool_doc.size()) + ")");
    }
    auto [base_pool, base_costs] = pool_doc.template instantiate<T>(opts.pool_index);

    Scenario<T> scenario;
    if (!cex_depth_path.empty())
        scenario.cex_depth.emplace(arb::events::load_cex_depth(cex_depth_path));
    std::vector<arb::Candle> candles;
    std::vector<arb::Event> events;
    if (opts.event_mode == "depth") {
        if (!scenario.cex_depth || !market_path.empty() || opts.candle_filter_pct != 0)
            throw std::invalid_argument("depth event_mode requires cex_depth_path and no market_path or candle_filter");
        if (!opts.observation_interval_s)
            throw std::invalid_argument("observation_interval_s must be positive");
        const auto& books = scenario.cex_depth->snapshots();
        constexpr uint64_t NS = 1'000'000'000ULL;
        const auto ceil_second = [](uint64_t ns) { return ns / NS + (ns % NS != 0); };
        const uint64_t begin = std::max(opts.start_ts, ceil_second(books.front().available_ns));
        const uint64_t end = opts.end_ts ? std::min(opts.end_ts, ceil_second(books.back().available_ns))
                                         : ceil_second(books.back().available_ns);
        if (begin > end) throw std::invalid_argument("No depth observations in requested time range");
        uint64_t count = (end-begin) / opts.observation_interval_s + 1;
        if (opts.max_candles) count = std::min(count, uint64_t(opts.max_candles));
        // Event candle indices are uint32_t.
        if (count > uint64_t(std::numeric_limits<uint32_t>::max()) + 1)
            throw std::invalid_argument("Depth observation indices exceed uint32_t");
        candles.reserve(count);
        events.reserve(count);
        size_t current = 0;
        for (uint64_t i = 0; i < count; ++i) {
            const uint64_t ts = begin + i*opts.observation_interval_s;
            while (current+1 < books.size() && books[current+1].available_ns <= ts*NS) ++current;
            const auto& book = books[current];
            const double bid = book.bids.front().price, ask = book.asks.front().price;
            const double mid = bid + (ask-bid)/2;
            events.push_back({ts, mid, 0.0, 0, 0.0, static_cast<uint32_t>(i)});
            // Internal trace rows only: no companion OHLC input or synthetic volume.
            candles.push_back({ts, mid, mid, mid, mid, 0.0});
        }
    } else if (opts.event_mode == "candle_path" || opts.event_mode == "mixed_depth") {
        scenario.candle_fallback = opts.event_mode == "mixed_depth";
        if (scenario.candle_fallback && (!scenario.cex_depth || market_path.empty()))
            throw std::invalid_argument("mixed_depth requires cex_depth_path and market_path");
        const double filter_squeeze = opts.candle_filter_pct > 0.0
            ? opts.candle_filter_pct / 100.0 : 0.999;
        candles = arb::load_candles(market_path, opts.max_candles, filter_squeeze, opts.start_ts);
        if (opts.end_ts > 0)
            candles.erase(std::remove_if(candles.begin(), candles.end(), [&](const arb::Candle& candle) {
                return candle.ts > opts.end_ts;
            }), candles.end());
        if (candles.empty()) throw std::runtime_error("No candles loaded for scenario '" + scenario_id + "'");
        events = scenario.candle_fallback
            ? arb::gen_mixed_depth_events(candles, *scenario.cex_depth,
                opts.observation_interval_s, opts.cex_depth_max_age_s, opts.start_ts)
            : arb::gen_events(candles);
    } else {
        throw std::invalid_argument("event_mode must be 'candle_path', 'depth', or 'mixed_depth'");
    }
    if (!price_feed_path.empty()) {
        auto feed_points = arb::price_feeds::load_price_feed_csv(price_feed_path);
        arb::price_feeds::attach_price_feed(events, feed_points);
    }

    scenario.id = scenario_id;
    scenario.candles = std::move(candles);
    scenario.events = arb::EventSoA::from_events(events);
    if (!observed_state_path.empty())
        scenario.observed_state.emplace(arb::events::load_observed_state_jsonl<T>(observed_state_path));
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
