// Checks of docs/arbitrage_rules.md on a synthetic pool (coin0 USD, coin1 at 77,235.68) and a 12 s tape.
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

// Events closing at relative moves from PRICE; zero native costs; active_2l at its standard settings if yb.
Run run(const std::vector<double>& moves, double entry_edge_bps, bool yb) {
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
    if (yb) { config.yb_mode = arb::harness::YbMode::Active2l; config.yb_releverage_fee = 0.013; config.yb_cash_multiplier = 3.0; }
    std::vector<arb::Candle> candles;
    const auto events = arb::EventSoA::from_events(arb::events::gen_candle_events(tape, candles, 0, 0, {}));
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
} // namespace

int main() {
    no_profitable_trade_remains();
    one_ordered_trade_per_actor_and_repeatable();
    std::cout << "test_arbitrage_rules: PASSED\n";
}
