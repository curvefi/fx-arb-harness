// Checks of docs/arbitrage_rules.md on a synthetic pool (coin0 USD, coin1 at 77,235.68) and a 12 s tape.
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <variant>
#include <vector>
#include "events/candle_tape.hpp"
#include "harness/event_loop.hpp"
#include "pools/twocrypto_fx/helpers.hpp"
#include "pools/twocrypto_fx/twocrypto.hpp"

namespace {
using Pool = arb::pools::twocrypto_fx::TwoCryptoPool<double>;
constexpr uint64_t TS = 1'779'753'600;
constexpr double PRICE = 77'235.68;

void require(bool condition, const char* message) {
    if (!condition) { std::cerr << "arbitrage rules failed: " << message << '\n'; std::exit(1); }
}

struct Run { arb::harness::EventLoopResult<double> result; Pool pool; std::vector<arb::harness::Action<double>> actions; };

// Events closing at relative moves from PRICE; zero native costs; active_2l at its standard settings if yb. Without a trade
// price (p_arb NaN at every event) neither actor may trade.
Run run(const std::vector<double>& moves, double entry_edge_bps, bool yb, bool touch = false, bool fast = false, double yb_fee = 0.013,
        bool no_trade_price = false) {
    Pool pool({1.0, 1.0}, 50'000.0, 1.1111111111e-8, 0.0146, 0.0170,
              0.054202748, 1e-10, 5e-3, 865.0, PRICE, 0.301010101, 0.0);
    pool.set_block_timestamp(TS - 1);
    pool.add_liquidity({43'596'754.65, 564.46165}, 0.0);
    arb::events::CandleTape tape;
    tape.t0 = TS;
    tape.bin_s = 12;
    for (const double move : moves) {
        for (auto* column : {&tape.open, &tape.high, &tape.low, &tape.close}) column->push_back(PRICE * (1 + move));
        tape.volume.push_back(1.0);
    }
    Run out{{}, std::move(pool), {}};
    arb::trading::Costs<double> costs;
    costs.arb_fee_bps = 0.0; costs.gas_coin0 = 0.0; costs.entry_edge_bps = entry_edge_bps;
    arb::harness::DonationCfg<double> donation; donation.apy = 0.05;
    arb::harness::IdleTickCfg<double> idle; idle.freq_s = 0;
    arb::harness::UserSwapCfg<double> user;
    arb::harness::RunConfig<double> config;
    if (yb) { config.yb_mode = arb::harness::YbMode::Active2l; config.yb_releverage_fee = yb_fee; config.yb_cash_multiplier = 3.0; }
    config.yb_self_touch = touch;
    if (fast) config.event_cursor = arb::harness::EventCursor::FastSkip;
    std::vector<arb::Candle> candles;
    auto events = arb::EventSoA::from_events(arb::events::gen_candle_events(tape, candles, 0, 0, {}));
    if (no_trade_price) events.p_arb.assign(events.size(), std::nan(""));
    out.result = arb::harness::run_event_loop(out.pool, events, costs, donation, idle, user, config, nullptr, 0, &out.actions);
    return out;
}

std::vector<double> random_moves(int bins, double width) {
    std::vector<double> moves;
    uint64_t state = 7;
    for (int bin = 0; bin < bins; ++bin) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        moves.push_back((static_cast<double>(state >> 40) / double(1ULL << 24) - 0.5) * width);
    }
    return moves;
}

// With entry_edge_bps = 0 and zero costs, what any size in either direction (a log grid over 1e-8 .. 1 of the
// input balance) can still earn at the event price: under 0.2% of the captured profit after one event (the pool
// fee is charged at the post-trade state, so the best single size leaves a small follow-up), nothing after two.
void no_profitable_trade_remains() {
    for (const double move : {0.05, -0.05}) {
        for (const int events : {1, 2}) {
            const auto after = run(std::vector<double>(events, move), 0.0, false);
            const double price = PRICE * (1 + move);
            require(after.result.metrics.trades == static_cast<size_t>(events), "the event price was not traded");
            double left = 0;
            for (size_t i = 0; i < 2; ++i) {
                for (double frac = 1e-8; frac < 1.0; frac *= 1.25) {
                    const double dx = after.pool.balances[i] * frac;
                    const double dy = arb::pools::twocrypto_fx::simulate_exchange_once(after.pool, i, 1 - i, dx).first;
                    left = std::max(left, i == 0 ? dy * price - dx : dy - dx * price);
                }
            }
            require(events == 1 ? left < 2e-3 * after.result.metrics.arb_pnl_coin0 : left == 0,
                    "a profitable trade was left in the pool");
        }
    }
}

// The native arbitrageur acts before the YieldBasis actor at each event, each at most once, and a second
// run of the same inputs repeats every trade and the final pool state exactly.
void one_ordered_trade_per_actor_and_repeatable() {
    const auto moves = random_moves(600, 0.12);
    const auto first = run(moves, 1.5, true), second = run(moves, 1.5, true);
    uint64_t ts = 0, both = 0;
    int native = 0, fills = 0;
    for (const auto& action : first.actions) {
        const auto* swap = std::get_if<arb::harness::ExchangeAction<double>>(&action);
        const auto* fill = std::get_if<arb::harness::InjectedLog<double>>(&action);
        if (!swap && !fill) continue;
        const uint64_t at = swap ? swap->ts : fill->ts;
        if (at != ts) { ts = at; native = fills = 0; }
        require(swap ? ++native == 1 && fills == 0 : ++fills == 1, "an actor traded twice or out of order in one event");
        both += native && fills;
    }
    require(both > 0 && first.result.metrics.trades > 1 && first.result.metrics.yb_2l_fires > 1,
            "the tape did not exercise both actors");
    require(first.result.metrics.trades == second.result.metrics.trades &&
            first.result.metrics.yb_2l_fires == second.result.metrics.yb_2l_fires &&
            first.result.metrics.arb_pnl_coin0 == second.result.metrics.arb_pnl_coin0 &&
            first.result.metrics.yb_levamm_profit_coin0 == second.result.metrics.yb_levamm_profit_coin0 &&
            first.actions.size() == second.actions.size() && first.pool.balances == second.pool.balances &&
            first.pool.D == second.pool.D && first.pool.cached_price_scale == second.pool.cached_price_scale &&
            first.pool.totalSupply == second.pool.totalSupply, "two runs of the same inputs differed");
}

// yb_self_touch (LEVAMM fee 0.2 %): after a jump the price rests, nobody touches the pool and the scale lags. The actor's
// own touch steps the scale and fills where the pool as it stands gives none; the skipping cursor repeats the scalar one.
void self_touch_fills_and_cursors_agree() {
    for (const double jump : {0.04, -0.04}) {
        std::vector<double> moves(40, 0.0);
        moves.insert(moves.end(), 600, jump);
        const auto off = run(moves, 1.5, true, false, false, 0.002), on = run(moves, 1.5, true, true, false, 0.002),
                   fast = run(moves, 1.5, true, true, true, 0.002);
        require(off.result.metrics.yb_touch_fires == 0 && on.result.metrics.yb_touch_fires > 1 &&
                on.result.metrics.yb_2l_fires == off.result.metrics.yb_2l_fires + on.result.metrics.yb_touch_fires,
                "the actor's touch opened no fill");
        require(on.result.metrics.yb_touch_fires == fast.result.metrics.yb_touch_fires &&
                on.result.metrics.yb_2l_fires == fast.result.metrics.yb_2l_fires && on.result.metrics.trades == fast.result.metrics.trades &&
                on.result.metrics.yb_levamm_profit_coin0 == fast.result.metrics.yb_levamm_profit_coin0 &&
                on.pool.balances == fast.pool.balances && on.pool.D == fast.pool.D &&
                on.pool.cached_price_scale == fast.pool.cached_price_scale && on.pool.totalSupply == fast.pool.totalSupply,
                "the skipping cursor differed from the scalar one with yb_self_touch");
        const auto silent = run(moves, 1.5, true, true, false, 0.002, true);
        require(silent.result.metrics.yb_2l_fires == 0 && silent.result.metrics.trades == 0,
                "an actor traded at an event without a trade price");
    }
}

// The aggregator's price_w(): the first write in a block stands, the next block writes again, and another writer's
// cache from the same block stands too.
void aggregator_writes_once_per_block() {
    arb::harness::OracleRate o{14400, 14400, 0};
    require(o.write(100, 0.98, 50) && o.own_rate == 0.98, "a first write in a block stores the live value");
    require(!o.write(100, 0.97, 50) && o.own_rate == 0.98, "a second write in the same block keeps the first");
    require(o.write(112, 0.97, 50) && o.own_rate == 0.97, "the next block writes again");
    require(!o.write(124, 0.96, 124) && o.own_rate == 0.97, "another writer's cache from the same block stands");
}
} // namespace

int main() {
    no_profitable_trade_remains();
    one_ordered_trade_per_actor_and_repeatable();
    self_touch_fills_and_cursors_agree();
    aggregator_writes_once_per_block();
    std::cout << "test_arbitrage_rules: PASSED\n";
}
