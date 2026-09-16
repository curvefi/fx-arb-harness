#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <variant>
#include <vector>

#include "events/cex_depth.hpp"
#include "events/loader.hpp"
#include "events/types.hpp"
#include "curve_fx_evaluator/trace.hpp"
#include "harness/event_loop.hpp"
#include "harness/logging.hpp"
#include "harness/yb_reference_2l.hpp"
#include "pools/twocrypto_fx/twocrypto.hpp"

namespace {

using Snapshot = arb::trading::CexDepthSnapshot;
using Tape = arb::events::CexDepthTape;
using Pool = arb::pools::twocrypto_fx::TwoCryptoPool<double>;
constexpr uint64_t TS = 1'779'753'600;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "depth integration failed: " << message << '\n';
        std::exit(1);
    }
}

Pool make_pool() {
    Pool pool({1.0, 1.0}, 50'000.0, 1.1111111111e-8, 0.0146, 0.0170,
              0.054202748, 1e-10, 5e-3, 865.0, 77'235.68, 0.301010101, 0.0);
    pool.set_block_timestamp(TS - 1);
    pool.add_liquidity({43'596'754.65, 564.46165}, 0.0);
    return pool;
}

struct Run {
    arb::harness::EventLoopResult<double> result;
    std::vector<arb::harness::Action<double>> actions;
};

Run run(const Tape* tape, arb::harness::EventCursor cursor, uint64_t max_age = 30) {
    auto pool = make_pool();
    const auto events = arb::EventSoA::from_events({
        {TS, 77'235.68, 0.0, 0, 1.0, 0},
        {TS, 77'235.68, 0.0, 0, 1.0, 0},
    });
    arb::trading::Costs<double> costs;
    costs.arb_fee_bps = 0.0;
    arb::harness::DonationCfg<double> donation;
    arb::harness::IdleTickCfg<double> idle;
    idle.freq_s = 0;
    arb::harness::UserSwapCfg<double> user;
    arb::harness::RunConfig<double> config;
    config.min_swap_frac = 1e-8;
    config.cex_depth = tape;
    config.cex_depth_max_age_s = max_age;
    config.event_cursor = cursor;
    config.metric_profile = cursor == arb::harness::EventCursor::ExactSkip
        ? arb::harness::MetricProfile::GridCore
        : arb::harness::MetricProfile::FullSummary;
    Run output;
    output.result = arb::harness::run_event_loop(
        pool, events, costs, donation, idle, user, config, nullptr, 0,
        &output.actions);
    return output;
}

void loader_and_cursor_are_causal() {
    const Tape tape({{10'000'000'000ULL, {{100,1}}, {{120,1}}},
                     {12'000'000'000ULL, {{101,2}}, {{121,2}}}});
    arb::events::CexDepthCursor<double> cursor(tape, 1);
    require(!cursor.advance(9), "future snapshot was visible");
    require(!cursor.advance_ns(9'999'999'999ULL),
            "nanosecond cursor exposed a future snapshot");
    require(cursor.advance(10) == 110.0, "snapshot midpoint was not selected");
    require(cursor.book().consume_sell(0.5), "depth could not be consumed");
    require(cursor.advance(10) && *cursor.book().bid_capacity() == 0.5,
            "repeated timestamp reset depletion");
    require(cursor.advance(12) == 111.0 && *cursor.book().bid_capacity() == 2.0,
            "new snapshot did not refresh depth");
    require(!cursor.advance(14), "stale snapshot remained executable");
    using Real = curve_fx::evaluator::RealT;
    arb::harness::YbReference2LRouteResult<Real> route;
    route.direction = 1;
    route.input = Real(2);
    route.output = Real(3);
    route.profit_coin0 = Real(1);
    route.lp_amount = Real(4);
    route.donation = Real(5);
    route.flash_amount = Real(6);
    std::vector<arb::harness::Action<Real>> actions;
    arb::harness::ActionLogger<Real>(&actions).log_yb_route(TS, route);
    const auto encoded = curve_fx::evaluator::serialize_actions_json(actions);
    require(encoded.find("\"type\":\"yb_route\"") != std::string::npos &&
                encoded.find("\"route\":\"virtual_pool\"") != std::string::npos &&
                encoded.find("\"flash_amount\":6") != std::string::npos,
            "committed VirtualPool route action schema was not serialized");
}

void event_loop_commits_one_shared_depth_hedge() {
    const Tape tape({Snapshot{
        TS * 1'000'000'000ULL,
        {{50'000.0, 1.0}},
        {{70'000.0, 0.001}},
    }});
    const auto output = run(&tape, arb::harness::EventCursor::Scalar);
    require(output.result.metrics.trades == 1 && output.actions.size() == 1,
            "same-snapshot depth was not shared across actions");
    const auto& action = std::get<arb::harness::ExchangeAction<double>>(output.actions.front());
    require(action.i == 1 && action.p_cex == 60'000.0,
            "depth midpoint did not drive the selected native trade");
    require(std::abs(action.profit_coin0 - (action.dy_after_fee - 70'000.0 * action.dx)) < 1e-8,
            "reported profit did not use executable ask cashflow");
    require(output.result.metrics.notional == action.dx * 60'000.0 &&
                output.result.metrics.arb_pnl_coin0 == action.profit_coin0,
            "depth trade did not reach run cash and size metrics");
}

void cursor_fallback_and_no_depth_default_are_stable() {
    const Tape tape({Snapshot{
        TS * 1'000'000'000ULL,
        {{50'000.0, 1.0}},
        {{70'000.0, 0.001}},
    }});
    const auto scalar = run(&tape, arb::harness::EventCursor::Scalar);
    const auto exact = run(&tape, arb::harness::EventCursor::ExactSkip);
    require(exact.result.metrics.trades == scalar.result.metrics.trades &&
                exact.result.metrics.arb_pnl_coin0 == scalar.result.metrics.arb_pnl_coin0,
            "depth-configured exact_skip did not fall back to scalar decisions");
    const auto legacy_default = run(nullptr, arb::harness::EventCursor::Scalar);
    const auto legacy_age_zero = run(nullptr, arb::harness::EventCursor::Scalar, 0);
    require(legacy_default.result.metrics.trades == legacy_age_zero.result.metrics.trades &&
                legacy_default.result.metrics.arb_pnl_coin0 == legacy_age_zero.result.metrics.arb_pnl_coin0,
            "no-depth defaults changed legacy execution");
    require(legacy_default.actions.size() == legacy_age_zero.actions.size(),
            "no-depth defaults changed the legacy action count");
}

void reference_actor_ignores_native_execution_costs() {
    const uint64_t wall = TS * 1'000'000'000ULL;
    const Tape tape({Snapshot{wall, {{10'000.0, 1'000.0}}, {{20'000.0, 1'000.0}}}});
    for (bool finite : {false, true}) for (double gas_coin0 : {0.0, 1e12}) {
        auto pool = make_pool();
        const auto events = arb::EventSoA::from_events({{TS, 15'000.0, 0.0, 0, 1e-20, 0}});
        arb::trading::Costs<double> costs;
        costs.arb_fee_bps = gas_coin0 == 0.0 ? 0.0 : 500.0;
        costs.gas_coin0 = gas_coin0;
        // A sub-minimum native cap isolates VP behavior from native costs.
        costs.use_volume_cap = true;
        arb::harness::DonationCfg<double> donation;
        donation.apy = 0.0145;
        arb::harness::IdleTickCfg<double> idle;
        idle.freq_s = 0;
        arb::harness::UserSwapCfg<double> user;
        arb::harness::RunConfig<double> config;
        config.yb_mode = arb::harness::YbMode::Reference2l;
        config.yb_cash_multiplier = 3.0;
        config.yb_releverage_fee = 0.012;
        config.cex_depth = finite ? &tape : nullptr;
        const auto result = arb::harness::run_event_loop(
            pool, events, costs, donation, idle, user, config);
        require(result.metrics.trades == 0, "cost fixture unexpectedly executed native arbitrage");
        require(result.yb_releverage_trades == 1,
                "native fees or gas changed VP execution");
    }
}

void minute_arbs_use_independent_depth_and_sequential_pool_state() {
    for (auto mode : {arb::harness::YbMode::Reference2l, arb::harness::YbMode::Active2l}) {
        auto pool = make_pool(), expected = pool;
        const Tape tape({Snapshot{TS * 1'000'000'000ULL, {{100'000., 1.}}, {{100'001., 1.}}},
                         Snapshot{(TS + 60) * 1'000'000'000ULL, {{100'000., 1.}}, {{100'001., 1.}}}});
        const auto events = arb::EventSoA::from_events({
            {TS, 100'000.5, 0., 0, 1., 0}, {TS + 60, 100'000.5, 0., 0, 1., 0}});
        arb::trading::Costs<double> costs;
        costs.arb_fee_bps = 0.;
        arb::harness::DonationCfg<double> donation;
        donation.apy = .0145;
        arb::harness::IdleTickCfg<double> idle;
        idle.freq_s = 0;
        arb::harness::UserSwapCfg<double> user;
        arb::harness::RunConfig<double> config;
        config.yb_mode = mode;
        config.yb_cash_multiplier = 3.;
        config.yb_releverage_fee = .012;
        config.cex_depth = &tape;
        config.dustswap_freq_s = 0;
        config.actor_timing_mode = arb::harness::ActorTimingMode::MinuteSequential;
        auto market = arb::harness::YbReference2LMarket<double>::fresh_2l(
            expected, donation.apy, config.yb_releverage_fee, TS, config.yb_cash_multiplier);
        auto active = arb::harness::Yb2LActor<double>::fresh_2l(
            expected, donation.apy, config.yb_releverage_fee, TS, config.yb_cash_multiplier);
        arb::harness::YbReference2LCosts<double> vp_costs;
        vp_costs.min_net_profit_coin0 = config.yb_min_net_profit_coin0;
        arb::events::CexDepthCursor<double> cursor(tape, 30);
        size_t expected_fires = 0;
        for (uint64_t ts : {TS, TS + 60}) {
            expected.set_block_timestamp(ts);
            expected.refresh_policy_context();
            const double mid = *cursor.advance(ts);
            auto native_book = cursor.book(), vp_book = cursor.book();
            const auto quote = arb::trading::decide_trade(expected, mid, costs,
                std::numeric_limits<double>::infinity(), config.min_swap_frac,
                config.max_swap_frac, 1., 1., static_cast<const double*>(nullptr),
                static_cast<const double*>(nullptr), static_cast<const std::array<double, 2>*>(nullptr), &native_book);
            require(quote.do_trade, "minute fixture native trade was not profitable");
            expected.exchange(quote.i, quote.j, quote.dx, quote.dy_after_fee);
            const bool fired = mode == arb::harness::YbMode::Reference2l
                ? market.execute_best(expected, mid, ts, vp_costs, true, &vp_book).committed
                : active.try_fire(expected, mid, ts, {}).fired;
            expected_fires += fired;
        }
        const auto result = arb::harness::run_event_loop(pool, events, costs, donation, idle, user, config);
        require(expected_fires > 0 && result.metrics.trades == 2 &&
                    result.metrics.yb_2l_fires == expected_fires,
                "sequential execution changed the selected YB actor's decisions");
        for (size_t i = 0; i < 2; ++i)
            require(std::abs(pool.balances[i] / expected.balances[i] - 1.) < 1e-12,
                    "minute mode differs from sequential pool execution with independent books");
        require(std::abs(pool.cached_price_scale / expected.cached_price_scale - 1.) < 1e-12,
                "minute mode price scale differs from carried sequential state");
    }
}

void runtime_cadence_matches_explicit_subsampling() {
    std::vector<arb::Event> dense;
    std::vector<Snapshot> snapshots;
    for (uint64_t offset=0;offset<=610;offset+=10) {
        dense.push_back({TS+offset,100'000.5,0.,0,1.,0});
        snapshots.push_back({(TS+offset)*1'000'000'000ULL,{{100'000.,1.}},{{100'001.,1.}}});
    }
    const Tape tape(std::move(snapshots));
    const std::vector<arb::Candle> candles{{TS,100'000.5,100'000.5,100'000.5,100'000.5,1.}};
    arb::harness::RunConfig<double> cfg;
    cfg.actor_timing_mode=arb::harness::ActorTimingMode::MinuteSequential;
    cfg.yb_mode=arb::harness::YbMode::Reference2l;
    cfg.yb_cash_multiplier=3.;cfg.cex_depth=&tape;cfg.dustswap_freq_s=0;
    cfg.detailed_log=true;cfg.detailed_interval=1;
    arb::trading::Costs<double> costs;costs.arb_fee_bps=0.;
    arb::harness::DonationCfg<double> donation;
    arb::harness::IdleTickCfg<double> idle;idle.freq_s=0;
    arb::harness::UserSwapCfg<double> user;user.freq_s=0;
    for (uint64_t interval:{10,30,60,300}) {
        cfg.observation_interval_s=interval;
        std::vector<arb::Event> selected;
        for (const auto& e:dense)if((e.ts-TS)%interval==0)selected.push_back(e);
        auto pool=make_pool(),expected=pool;
        std::vector<arb::harness::DetailedEntry<double>> trace;
        const auto actual=arb::harness::run_event_loop<double>(pool,arb::EventSoA::from_events(dense),costs,donation,idle,user,cfg,&candles,0,nullptr,&trace);
        const auto reference=arb::harness::run_event_loop(expected,arb::EventSoA::from_events(selected),costs,donation,idle,user,cfg);
        require(actual.metrics.trades>0 && actual.metrics.trades==reference.metrics.trades &&
                actual.metrics.yb_2l_fires==reference.metrics.yb_2l_fires && pool.balances==expected.balances &&
                pool.cached_price_scale==expected.cached_price_scale,
                "runtime cadence differs from explicit source subsampling");
        require(trace.size()==selected.size(),"unselected observations emitted trace rows");
    }
}

void mixed_events_select_candles_only_when_depth_is_unavailable() {
    const std::vector<arb::Candle> source{{10,100,120,80,100,8},
        {30,100,120,80,100,8}, {50,100,120,80,100,8}, {70,100,120,80,100,8}};
    const Tape tape({{10'500'000'000ULL, {{99,1}}, {{101,1}}},
                     {60'000'000'000ULL, {{109,1}}, {{111,1}}}});
    auto candles = source;
    // Clock 11,21,...; the first book expires at 30.5, the second at 80.
    const auto mixed = arb::gen_mixed_depth_events(candles, tape, 10, 20);
    std::vector<uint64_t> times;
    for (const auto& event : mixed) times.push_back(event.ts);
    require(times == std::vector<uint64_t>({5,11,21,35,45,55,61,71}),
            "mixed clock used future/stale depth or dropped candle fallback");
    require(mixed[0].p_cex == 120 && mixed[0].volume == 4 &&
                mixed[1].p_cex == 100 && mixed[1].volume == 0 &&
                candles[mixed[1].candle_idx].close == 100,
            "mixed event price, volume or trace source changed");
    const Tape boundary({{5'000'000'000ULL, {{99,1}}, {{101,1}}}});
    candles = source;
    const auto exact = arb::gen_mixed_depth_events(candles, boundary, 10, 20);
    times.clear();
    for (const auto& event : exact) times.push_back(event.ts);
    require(times == std::vector<uint64_t>({5,15,25,35,45,55,65,75}) &&
                exact[2].volume == 0 && exact[3].volume == 4,
            "freshness equality or depth/candle tie rule changed");
}

void mixed_fallback_retains_candle_execution_and_shared_depth() {
    for (auto mode : {arb::harness::YbMode::Off, arb::harness::YbMode::Active2l,
                      arb::harness::YbMode::Reference2l}) {
        for (int source : {0, 1, 2}) { // Future, stale, and usable books.
            const uint64_t publication = TS + (source == 0 ? 100 : 0);
            const Tape tape({Snapshot{publication * 1'000'000'000ULL,
                {{100'000.,1.}}, {{100'001.,1.}}}});
            auto flat_pool = make_pool(), mixed_pool = flat_pool;
            const auto events = arb::EventSoA::from_events({
                {TS+31,100'000.5,0.,0,1.,0}, {TS+41,100'000.5,0.,0,1.,0}});
            arb::trading::Costs<double> costs; costs.arb_fee_bps = 0.;
            arb::harness::DonationCfg<double> donation; donation.apy = .0145;
            auto mixed_donation = donation;
            arb::harness::IdleTickCfg<double> idle; idle.freq_s = 0;
            arb::harness::UserSwapCfg<double> user;
            arb::harness::RunConfig<double> cfg;
            cfg.yb_mode = mode; cfg.yb_cash_multiplier = 3.;
            cfg.cex_depth_max_age_s = source == 2 ? 60 : 30;
            if (source == 2) cfg.cex_depth = &tape;
            const auto expected = arb::harness::run_event_loop(
                flat_pool, events, costs, donation, idle, user, cfg);
            cfg.cex_depth = &tape; cfg.candle_fallback = true;
            const auto actual = arb::harness::run_event_loop(
                mixed_pool, events, costs, mixed_donation, idle, user, cfg);
            if (mode != arb::harness::YbMode::Off && source != 2)
                require(actual.yb_releverage_trades > 0, "fallback fixture did not exercise YB execution");
            require(actual.metrics.trades > 0 && flat_pool.balances == mixed_pool.balances &&
                        flat_pool.cached_price_scale == mixed_pool.cached_price_scale &&
                        expected.metrics.arb_pnl_coin0 == actual.metrics.arb_pnl_coin0 &&
                        expected.yb_releverage_final_growth == actual.yb_releverage_final_growth &&
                        expected.yb_releverage_trades == actual.yb_releverage_trades,
                    "mixed execution diverged from its candle or finite-depth baseline");
        }
    }
}

} // namespace

void monthly_numpy_archive_matches_python_fixture() {
    const auto directory = std::filesystem::path(__FILE__).parent_path()/"fixtures";
    const auto tape = arb::events::load_cex_depth((directory/"depth-v1.npz").string());
    const auto& rows = tape.snapshots();
    require(rows.size() == 2 && rows[0].available_ns == 10'000'000'000ULL &&
            rows[1].available_ns == 20'000'000'000ULL, "NPZ clock changed");
    require(rows[0].bids.size() == 1 && rows[0].asks.size() == 1 &&
            rows[0].bids[0].price == 99.12345678901234 &&
            rows[1].asks[0].quantity == 0.0987654321098765,
            "NPZ float64 levels changed");
    const auto flexible = arb::events::load_cex_depth((directory/"depth-v2.npz").string());
    require(flexible.snapshots()[0].bids.size() == 17, "NPZ truncated variable levels");
    arb::events::CexDepthCursor<double> cursor(flexible, 30);
    require(!cursor.advance(1'700'000'000), "fractional publication leaked into preceding second");
    require(cursor.advance(1'700'000'001) == 1.0, "first causal midpoint changed");
    require(std::abs(*cursor.book().buy_base(2.0)-2.035) < 1e-12,
            "NPZ lost linear interpolation or zero-quantity gap");
    require(cursor.advance(1'700'000'013) == 1.02, "irregular publication did not refresh book");
    require(!cursor.advance(1'700'000'061), "irregular stale book remained executable");
    bool rejected = false;
    try {
        (void)arb::events::load_cex_depth((directory/"depth-invalid-count.npz").string());
    } catch (const std::exception&) { rejected = true; }
    require(rejected, "NPZ accepted an out-of-range level count");
}

int main() {
    monthly_numpy_archive_matches_python_fixture();
    loader_and_cursor_are_causal();
    event_loop_commits_one_shared_depth_hedge();
    cursor_fallback_and_no_depth_default_are_stable();
    reference_actor_ignores_native_execution_costs();
    minute_arbs_use_independent_depth_and_sequential_pool_state();
    runtime_cadence_matches_explicit_subsampling();
    mixed_events_select_candles_only_when_depth_is_unavailable();
    mixed_fallback_retains_candle_execution_and_shared_depth();
    std::cout << "test_depth_integration: PASSED\n";
}
