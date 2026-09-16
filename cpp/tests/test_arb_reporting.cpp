#include <cmath>
#include <stdexcept>
#include "curve_fx_evaluator/compiled_grid.hpp"
#include "harness/event_loop.hpp"

namespace fx = arb::pools::twocrypto_fx;

void require_at(bool value, int line) {
    if (!value) throw std::runtime_error("arb reporting regression at line " + std::to_string(line));
}
#define require(value) require_at((value), __LINE__)

void policy_fees() {
    using Policy = fx::ChallengeFeePolicy<double>;
    fx::PolicyConfig<double> params;
    params.n_params = 3;
    params.params = {0.02, 0.5, 0.03};
    fx::PolicyPoolConfig<double> config;
    Policy::validate_params(params, config);
    Policy::State state{{100, 100}, 1};
    fx::PolicyResearchContext<double> report{100, 1, 1.1, 100};
    // Pay 10 coin0, receive 10 coin1 worth 11 coin0: edge = 1/11.
    const std::array<double, 2> endpoint{110, 90};
    require(Policy::get_fee(state, params, config, report, endpoint) == 0.03);
    params.params[0] = 0.1;
    require(Policy::get_fee(state, params, config, report, endpoint) == 0.03);
    require(Policy::fee_floor(params, config, 0.0) == 0.03);
    params.params[2] = 0.2;
    require(Policy::get_fee(state, params, config, report, endpoint) == 0.1);
    require(Policy::context_fee_floor(state, params, config, report, 0) == 0.1);
    params.params[0] = 0;
    require(std::abs(Policy::get_fee(state, params, config, report, endpoint) - 0.5/11) < 1e-14);
    params.params[1] = 0;
    require(Policy::get_fee(state, params, config, report, endpoint) == 1e-5);
    params.params[2] = 0.03;
    report.price_feed_timestamp = 99;
    require(Policy::context_fee_floor(state, params, config, report, 0) == 0.03);
    require(Policy::get_fee(state, params, config, report, endpoint) == 0.03);
    report.price_feed = 0;
    require(Policy::context_fee_floor(state, params, config, report, 1) == 0.03);
    require(Policy::get_fee(state, params, config, report, endpoint) == 0.03);
    require(Policy::get_price_scale(state, report, params, config) == 0);
    // The same fee floor on the contract lattice: WAD balances, 1e10 fees.
    using U = fx::uint256;
    using UintPolicy = fx::ChallengeFeePolicy<U>;
    const U wad("1000000000000000000");
    fx::PolicyConfig<U> up;
    up.n_params = 3;
    up.params = {U(200000000), wad / 2, U(300000000)};
    fx::PolicyPoolConfig<U> uc;
    uc.precision = wad;
    uc.fee_precision = U(10000000000ULL);
    UintPolicy::State us{{U(100)*wad,U(100)*wad},wad};
    fx::PolicyResearchContext<U> ur{100,wad,wad*11/10,100};
    require(UintPolicy::get_fee(us,up,uc,ur,{U(110)*wad,U(90)*wad}) == U(300000000));
    up.params[0] = U(1000000000);
    require(UintPolicy::get_fee(us,up,uc,ur,{U(110)*wad,U(90)*wad}) == U(300000000));
}

void reporting_grid_and_execution() {
    const auto raw = boost::json::parse(R"({
        "candidate_defaults":{"policy_params":[0.001,0.5,1],"pool":{}},
        "axes":{"pool.run.arb_report_rate":[0,0.5,1]},
        "axis_order":["pool.run.arb_report_rate"],"shape":[3]
    })").as_object();
    std::string error;
    auto grid = curve_fx::evaluator::CompiledGrid<double>::compile(raw, 3, error);
    if (!grid) throw std::runtime_error(error);
    std::vector<curve_fx::evaluator::EvaluationCandidate<double>> candidates;
    if (!grid->materialize_ranges(boost::json::parse("[[0,3]]").as_array(), candidates, error))
        throw std::runtime_error(error);
    std::vector<arb::Event> rows;
    for (unsigned i = 0; i < 100; ++i) {
        const double price = i % 2 ? 0.9 : 1.1;
        rows.push_back({100+i, price, price, 100+i, 1e9, 0});
    }
    const auto events = arb::EventSoA::from_events(rows);
    std::array<uint64_t, 6> counts{};
    std::array<double, 2> mixed_balances{};
    for (size_t index = 0; index < counts.size(); ++index) {
        const size_t candidate_index = index < 3 ? index : (index == 4 ? 2 : 1);
        const auto& candidate = candidates[candidate_index];
        fx::PolicyConfig<double> params;
        params.n_params = 3;
        params.params = {0.001, 0.5, 1};
        fx::TwoCryptoPool<double> pool({1,1}, 50000, 1e-8, .01, .01, .05,
            1e-10, .005, 865, 1, .3, 0, fx::PolicyKind::Compiled, params);
        pool.set_block_timestamp(99);
        pool.add_liquidity({1000000,1000000}, 0);
        arb::harness::RunConfig<double> cfg;
        cfg.arb_report_rate = *candidate.typed_pool_override->arb_report_rate;
        require(cfg.arb_report_rate == candidate_index * .5);
        arb::harness::DonationCfg<double> donation;
        arb::harness::IdleTickCfg<double> idle;
        idle.freq_s = 0;
        arb::harness::UserSwapCfg<double> user;
        arb::trading::Costs<double> costs;
        costs.arb_fee_bps = 0;
        std::vector<arb::harness::Action<double>> actions;
        auto tape = events;
        if (index == 3) {
            tape.p_price_feed.clear();
            tape.price_feed_ts.clear();
        }
        if (index == 4) for (auto& ts : tape.price_feed_ts) --ts;
        cfg.event_cursor = index == 5 ? arb::harness::EventCursor::ExactSkip
                                     : arb::harness::EventCursor::Scalar;
        const auto result = index == 5
            ? arb::harness::run_event_loop_impl<false,true>(pool, tape, costs, donation, idle, user, cfg)
            : arb::harness::run_event_loop_impl<false,false>(
                pool, tape, costs, donation, idle, user, cfg, nullptr, 0, &actions);
        counts[index] = result.metrics.trades;
        if (index == 1) mixed_balances = pool.balances;
        if (index == 3 || index == 5) require(mixed_balances == pool.balances);
        require(pool.policy.research.price_feed == 0);
        for (const auto& action : actions) {
            const auto& swap = std::get<arb::harness::ExchangeAction<double>>(action);
            require(arb::harness::arb_brings_report(swap.ts - 100, cfg.arb_report_rate));
            require(swap.profit_coin0 >= 0);
        }
    }
    require(counts[0] == 0 && counts[1] > 0 && counts[1] < counts[2]);
    require(counts[3] == counts[1] && counts[4] == 0);
    require(counts[5] == counts[1]);
}

int main() {
    policy_fees();
    reporting_grid_and_execution();
}
