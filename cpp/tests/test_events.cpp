#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <variant>
#include <vector>
#include "events/block_tape.hpp"
#include "events/candle_tape.hpp"
#include "harness/event_loop.hpp"
#include "pools/twocrypto_fx/twocrypto.hpp"

namespace {
using Tape = arb::events::CandleTape;
using Pool = arb::pools::twocrypto_fx::TwoCryptoPool<double>;
constexpr uint64_t TS = 1'779'753'600;

void require(bool condition, const char* message) {
    if (!condition) { std::cerr << "events failed: " << message << '\n'; std::exit(1); }
}

Pool make_pool() {  // Mid fee 1.46%: the event price must clear roughly that band.
    Pool pool({1.0, 1.0}, 50'000.0, 1.1111111111e-8, 0.0146, 0.0170,
              0.054202748, 1e-10, 5e-3, 865.0, 77'235.68, 0.301010101, 0.0);
    pool.set_block_timestamp(TS - 1);
    pool.add_liquidity({43'596'754.65, 564.46165}, 0.0);
    return pool;
}

// Candle events closing at relative moves from the pool price.
arb::EventSoA closes(const std::vector<double>& moves) {
    Tape tape;
    tape.t0 = TS;
    tape.bin_s = 12;
    for (const double move : moves) {
        for (auto* column : {&tape.open, &tape.high, &tape.low, &tape.close}) column->push_back(77'235.68 * (1 + move));
        tape.volume.push_back(1.0);
    }
    std::vector<arb::Candle> candles;
    return arb::EventSoA::from_events(arb::events::gen_candle_events(tape, candles, 0, 0, {}));
}

struct Run { arb::harness::EventLoopResult<double> result; Pool pool; std::vector<arb::harness::Action<double>> actions; };

// Blocks whose price one second after the block is a relative move from the pool price.
arb::EventSoA blocks(const std::vector<double>& moves) {
    arb::events::BlockTape tape;
    tape.t0 = TS - TS % 12 + 11;
    tape.block_s = 12;
    for (const double move : moves) {
        tape.price.push_back({77'235.68, 77'235.68 * (1 + move), 77'235.68, 77'235.68, 77'235.68});
        tape.volume.push_back({1.0, 1.0, 1.0});
    }
    std::vector<arb::Candle> candles;
    return arb::events::gen_block_events(tape, 1, candles, 0, 0, {});
}

Run run_arb(const arb::EventSoA& events, arb::harness::EventCursor cursor = arb::harness::EventCursor::Scalar,
            double entry_edge_bps = 0.0) {
    Run run{{}, make_pool(), {}};
    arb::trading::Costs<double> costs; costs.arb_fee_bps = 0.0; costs.entry_edge_bps = entry_edge_bps;
    arb::harness::DonationCfg<double> donation;
    arb::harness::IdleTickCfg<double> idle; idle.freq_s = 0;
    arb::harness::UserSwapCfg<double> user;
    arb::harness::RunConfig<double> config;
    config.min_swap_frac = 1e-8; config.event_cursor = cursor;
    run.result = arb::harness::run_event_loop(run.pool, events, costs, donation, idle, user, config, nullptr, 0, &run.actions);
    return run;
}

void candles_become_a_causal_bin_end_clock() {
    // Candles keep their clock; a missing candle is a bin without a price.
    const uint64_t t0 = 1'700'000'040;
    const auto tape = arb::events::make_candle_tape(
        {{t0, 100, 101, 99.5, 100, 12}, {t0 + 24, 100, 100, 99.9, 99.9, 1}, {t0 + 36, 100, 100, 99, 99, 1}});
    require(tape.bin_s == 12 && tape.bins() == 4 && tape.close[0] == 100 && tape.volume[0] == 12 &&
            std::isnan(tape.close[1]), "candle tape lost its clock or prices");
    std::vector<arb::Candle> candles;
    const auto events = arb::events::gen_candle_events(tape, candles, 0, 0, {});
    require(events.size() == 4 && events[0].ts == t0 + 12 && events[2].ts == t0 + 36 &&
            events[0].p_cex == 100.0 && events[1].p_cex == 100.0 && events[2].p_cex == 99.9 && candles[1].volume == 0.0,
            "bin-end clock, carried mark or trace candles changed");
    // An excluded first bin neither emits an event nor marks the next, empty one.
    candles.clear();
    require(arb::events::gen_candle_events(tape, candles, 0, 0, {{t0, t0 + 6}}).size() == 2,
            "an excluded candle leaked into the mark");
}

// A packed block tape (fixtures/README.md) keeps its 12 s clock and prices each block one second after it.
void block_tape_loads_and_prices_at_the_settle_offset() {
    const auto path = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "block-tape-v1.npz";
    const auto tape = arb::events::load_block_tape(path.string());
    std::vector<arb::Candle> candles;
    const auto events = arb::events::gen_block_events(tape, 1, candles, 0, 0, {});
    require(tape.blocks() == 4 && events.size() == 4 && events.ts[0] == 1'700'000'003, "block tape lost its clock");
    for (size_t k = 0; k < events.size(); ++k)
        require(events.ts[k] == events.ts[0] + 12 * k && events.p_cex[k] == tape.price[k][1] &&
                events.p_cex[k] == 100.0 + double(k) + 0.1, "block event clock or settle-offset price changed");
}

void taker_trades_at_the_event_price() {
    require(run_arb(closes({0.005})).result.metrics.trades == 0, "a price inside the pool fee band was traded");
    const auto up = run_arb(closes({0.05}));
    const auto& bought = std::get<arb::harness::ExchangeAction<double>>(up.actions.front());
    require(up.result.metrics.trades == 1 && bought.i == 0 && bought.profit_coin0 > 0, "a price above the band was not bought");
    const auto down = run_arb(closes({-0.05}));
    require(down.result.metrics.trades == 1 && std::get<arb::harness::ExchangeAction<double>>(down.actions.front()).i == 1,
            "a price below the band was not sold");
    // An entry threshold admits only a larger first-unit edge: a 5% move past a ~1.46% fee is ~341 bp.
    for (const auto& [threshold, trades] : {std::pair{300.0, 1ULL}, std::pair{400.0, 0ULL}})
        require(run_arb(closes({0.05}), arb::harness::EventCursor::Scalar, threshold).result.metrics.trades == trades,
                "the entry threshold misjudged the first unit's edge");
}

void fast_skip_matches_scalar() {
    std::vector<double> moves;
    uint64_t state = 7;
    for (int bin = 0; bin < 240; ++bin) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        moves.push_back((static_cast<double>(state >> 40) / double(1ULL << 24) - 0.5) * 0.06);
    }
    for (const auto& events : {closes(moves), blocks(moves)}) {
        const auto scalar = run_arb(events), skipped = run_arb(events, arb::harness::EventCursor::FastSkip);
        require(scalar.result.metrics.trades > 1 && scalar.result.metrics.trades == skipped.result.metrics.trades &&
                scalar.pool.balances == skipped.pool.balances &&
                scalar.pool.cached_price_scale == skipped.pool.cached_price_scale, "fast skip diverged");
    }
}
} // namespace

int main() {
    candles_become_a_causal_bin_end_clock();
    block_tape_loads_and_prices_at_the_settle_offset();
    taker_trades_at_the_event_price();
    fast_skip_matches_scalar();
    std::cout << "test_events: PASSED\n";
}
