#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <variant>
#include <vector>
#include "events/block_tape.hpp"
#include "events/candle_tape.hpp"
#include "harness/event_loop.hpp"
#include "harness/report_window.hpp"
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
            double entry_edge_bps = 0.0, int delay_s = -1) {
    Run run{{}, make_pool(), {}};
    arb::trading::Costs<double> costs; costs.arb_fee_bps = 0.0; costs.entry_edge_bps = entry_edge_bps;
    arb::harness::DonationCfg<double> donation;
    arb::harness::IdleTickCfg<double> idle; idle.freq_s = 0;
    arb::harness::UserSwapCfg<double> user;
    arb::harness::RunConfig<double> config;
    config.min_swap_frac = 1e-8; config.event_cursor = cursor; config.arb_settle_offset_s = delay_s;
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

// A run priced at its own delay after the block (0, 1 or 2 s) on a session opened at 1 s trades exactly as a
// session opened at that delay. Prices in coin 0 per coin 1; each delay sees its own +-3% moves.
void a_run_takes_its_own_price_delay() {
    arb::events::BlockTape tape;
    tape.t0 = TS - TS % 12 + 11;
    tape.block_s = 12;
    uint64_t state = 11;
    for (int block = 0; block < 120; ++block) {
        std::array<double, arb::events::BLOCK_PRICE_OFFSETS> row{};
        for (double& price : row) {
            state = state * 6364136223846793005ULL + 1442695040888963407ULL;
            price = 77'235.68 * (1 + (static_cast<double>(state >> 40) / double(1ULL << 24) - 0.5) * 0.06);
        }
        tape.price.push_back(row);
        tape.volume.push_back({1.0, 1.0, 1.0});
    }
    std::vector<arb::Candle> candles;
    const auto session = arb::events::gen_block_events(tape, 1, candles, 0, 0, {});
    size_t trades[3]{};
    for (int delay = 0; delay < 3; ++delay) {
        candles.clear();
        const auto direct = run_arb(arb::events::gen_block_events(tape, size_t(delay), candles, 0, 0, {}));
        for (const auto cursor : {arb::harness::EventCursor::Scalar, arb::harness::EventCursor::FastSkip}) {
            const auto own = run_arb(session, cursor, 0.0, delay);
            require(own.result.metrics.trades == direct.result.metrics.trades && own.pool.balances == direct.pool.balances &&
                    own.pool.cached_price_scale == direct.pool.cached_price_scale, "a run's own price delay diverged");
        }
        trades[delay] = direct.result.metrics.trades;
    }
    require(trades[0] > 1 && (trades[0] != trades[1] || trades[1] != trades[2]), "the delays did not differ");
}
// The trade price (p_arb) replaces the event price for the actors' trades, here the native arbitrageur's; NaN: no trade
// there (the YieldBasis actor: test_arbitrage_rules).
void trade_price_replaces_the_event_price() {
    const auto plain = blocks({0.05, -0.05, 0.05});
    auto same = plain, none = plain, middle = plain;
    same.p_arb = plain.p_cex;
    none.p_arb.assign(plain.size(), std::nan(""));
    middle.p_arb = none.p_arb;
    middle.p_arb[1] = plain.p_cex[1];
    for (const auto cursor : {arb::harness::EventCursor::Scalar, arb::harness::EventCursor::FastSkip}) {
        const auto base = run_arb(plain, cursor), own = run_arb(same, cursor);
        require(base.result.metrics.trades == 3 && own.result.metrics.trades == 3 && own.pool.balances == base.pool.balances,
                "an arbitrage price equal to the event price changed the run");
        require(run_arb(none, cursor).result.metrics.trades == 0, "an arbitrageur without a price traded");
        const auto one = run_arb(middle, cursor);
        require(one.result.metrics.trades == 1 && std::get<arb::harness::ExchangeAction<double>>(one.actions.front()).i == 1,
                "the arbitrageur did not trade only where it has a price");
        // A flat market with the arbitrageur's own price 5% above it: it buys coin 1 once, on its own price.
        auto apart = blocks({0.0, 0.0});
        apart.p_arb = {apart.p_cex[0] * 1.05, std::nan("")};
        const auto own_view = run_arb(apart, cursor);
        require(run_arb(blocks({0.0, 0.0}), cursor).result.metrics.trades == 0 && own_view.result.metrics.trades == 1 &&
                std::get<arb::harness::ExchangeAction<double>>(own_view.actions.front()).i == 0,
                "the arbitrageur did not trade on its own price");
    }
}
// The tape rate oracle reads the newer cache until it is older than the limit (shorter off peg), then the live value,
// which only a commit stores; another writer's newer cache wins again.
void oracle_rate_reads_the_cache_until_its_age_limit() {
    arb::harness::OracleRate o{3600, 180, 0.002};
    auto r = o.preview(10'000, 0.9985, 0.999, 7'000);
    require(r.rate == 0.999 && !r.refresh, "a cache younger than the calm limit was not read");
    r = o.preview(10'601, 0.9985, 0.999, 7'000);
    require(r.rate == 0.9985 && r.refresh, "a cache older than the calm limit was not refreshed");
    r = o.preview(10'700, 0.9985, 0.999, 7'000);
    require(r.rate == 0.9985 && r.refresh, "a preview stored the refresh");
    o.commit(10'700, 0.9985);
    r = o.preview(11'000, 0.990, 0.999, 7'000);
    require(r.rate == 0.9985 && !r.refresh, "the own write was not the newer cache");
    o.commit(11'000, 0.990);  // 100 bp off peg: the 180 s limit applies
    require(o.preview(11'180, 0.995, 0.999, 7'000).rate == 0.990 && o.preview(11'181, 0.995, 0.999, 7'000).refresh,
            "the off-peg limit did not apply");
    r = o.preview(11'100, 0.995, 0.997, 11'050);
    require(r.rate == 0.997 && !r.refresh, "another writer's newer cache was not read");
    o.commit(11'200, 0.996);  // another writer's cache stamped in the same block keeps ours
    r = o.preview(11'212, 0.98, 0.995, 11'200);
    require(r.rate == 0.996 && !r.refresh, "a same-block cache replaced our write");
}

// Report access: 0 the latest report at or before the block; 2000 + lead the menu [block - 12 s, block + lead];
// 3000 + lead the latest at or before the block plus (block, block + lead].
void report_offsets_select_their_windows() {
    arb::EventSoA ev;
    ev.report_ts = {80.0, 87.0, 88.0, 99.0, 100.0, 101.0, 102.0, 103.0, 104.0};
    ev.report_prices = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0};
    const auto seen = [&](size_t offset) {
        std::vector<double> out;
        arb::harness::ReportWindow(ev, 0.0).at_offset(100.0, offset, [&](double p, double) { out.push_back(p); });
        return out;
    };
    require(seen(0) == std::vector<double>{5.0}, "offset 0 is the latest report at or before the block");
    require(seen(2003) == std::vector<double>{3.0, 4.0, 5.0, 6.0, 7.0, 8.0}, "the menu holds [block - 12 s, block + 3 s]");
    require(seen(3002) == std::vector<double>{5.0, 6.0, 7.0}, "the fresh menu holds the latest at the block and (block, block + 2 s]");
    require(seen(3000) == seen(0), "lead 0 of the fresh menu is offset 0");
    double last_ts = 0.0;
    arb::harness::ReportWindow(ev, 0.0).at_offset(100.0, 3002, [&](double, double t) { last_ts = t; });
    require(last_ts == 102.0, "the fresh menu keeps the report's own timestamp and stops at block + 2 s");
}
} // namespace

int main() {
    oracle_rate_reads_the_cache_until_its_age_limit();
    trade_price_replaces_the_event_price();
    a_run_takes_its_own_price_delay();
    candles_become_a_causal_bin_end_clock();
    block_tape_loads_and_prices_at_the_settle_offset();
    taker_trades_at_the_event_price();
    fast_skip_matches_scalar();
    report_offsets_select_their_windows();
    std::cout << "test_events: PASSED\n";
}
