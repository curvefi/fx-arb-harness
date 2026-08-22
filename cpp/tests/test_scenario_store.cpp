#include <iostream>
#include <string>

#include "curve_fx_evaluator/evaluator.hpp"
#include "harness/runner.hpp"

namespace {

void require(bool condition, const char* msg) {
    if (!condition) {
        std::cerr << "FAIL: " << msg << std::endl;
        std::exit(1);
    }
}

} // namespace

int main() {
    using namespace curve_fx::evaluator;

    arb::pools::PoolInit<double> pool_init;
    pool_init.start_ts = 100;
    pool_init.initial_liq = {1'000'000.0, 500'000.0};
    arb::EventSoA events;
    events.ts = {200, 300};
    events.p_cex = {1.0, 2.0};
    events.volume = {0.0, 0.0};
    events.candle_idx = {0, 1};
    arb::trading::Costs<double> costs;
    arb::harness::RunConfig<double> cfg;
    cfg.dustswap_freq_s = 0;
    cfg.start_ts = 250;
    auto overridden = arb::harness::run_single_pool(
        pool_init, costs, events, cfg);
    cfg.start_ts = 0;
    auto templated = arb::harness::run_single_pool(
        pool_init, costs, events, cfg);
    require(overridden.success && templated.success, "runner start test must succeed");
    require(overridden.tvl_start > templated.tvl_start + 100'000.0,
        "nonzero run start must override template start");

    std::cout << "test_scenario_store: PASSED\n";
    return 0;
}
