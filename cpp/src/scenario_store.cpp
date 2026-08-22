#include "curve_fx_evaluator/evaluator.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace curve_fx::evaluator {

namespace {

bool is_safe_scenario_id(const std::string& value) {
    if (value.empty()) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isalnum(c) != 0 || c == '-' || c == '_';
    });
}

} // namespace

template <typename T>
void ScenarioStore<T>::load(
    const std::string& template_path,
    const std::string& scenario_id,
    const std::string& market_path,
    const std::string& chainlink_path,
    const ScenarioLoadOptions& opts
) {
    auto pool_doc = arb::pools::PoolConfigDocument::from_file(template_path);
    if (opts.pool_index >= pool_doc.size()) {
        throw std::runtime_error("Template pool_index " + std::to_string(opts.pool_index) +
                                 " out of range (total: " + std::to_string(pool_doc.size()) + ")");
    }
    auto [base_pool, base_costs] = pool_doc.template instantiate<T>(opts.pool_index);

    if (!is_safe_scenario_id(scenario_id)) {
        throw std::runtime_error("Scenario id contains unsafe characters: " + scenario_id);
    }
    if (!fs::exists(market_path) || fs::is_directory(market_path)) {
        throw std::runtime_error(
            "Candles file not found for scenario '" + scenario_id + "': " + market_path);
    }

    const double filter_squeeze = opts.candle_filter_pct > 0.0
        ? opts.candle_filter_pct / 100.0
        : 0.999;
    auto candles = arb::load_candles(
        market_path, opts.max_candles, filter_squeeze, opts.start_ts);
    if (opts.end_ts > 0) {
        candles.erase(
            std::remove_if(candles.begin(), candles.end(), [&](const arb::Candle& candle) {
                return candle.ts > opts.end_ts;
            }),
            candles.end()
        );
    }
    if (candles.empty()) {
        throw std::runtime_error(
            "No candles loaded for scenario '" + scenario_id + "' from " + market_path);
    }

    auto events = arb::gen_events(candles);
    if (!chainlink_path.empty()) {
        if (!fs::exists(chainlink_path) || fs::is_directory(chainlink_path)) {
            throw std::runtime_error(
                "Chainlink file not found for scenario '" + scenario_id + "': " + chainlink_path);
        }
        auto cl_points = arb::oracles::load_chainlink_csv(chainlink_path);
        arb::oracles::attach_chainlink_prices(events, cl_points);
    }

    Scenario<T> scenario;
    scenario.id = scenario_id;
    scenario.candle_path = market_path;
    scenario.chainlink_path = chainlink_path;
    scenario.candles = std::move(candles);
    scenario.events = arb::EventSoA::from_events(events);
    scenario.base_pool = base_pool;
    scenario.base_costs = base_costs;
    scenario.start_ts = scenario.candles.front().ts;

    scenarios_.clear();
    scenarios_.push_back(std::move(scenario));
}

// Explicit template instantiations
template class ScenarioStore<double>;
template class ScenarioStore<long double>;
template class ScenarioStore<float>;

} // namespace curve_fx::evaluator
