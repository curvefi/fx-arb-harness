#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <variant>
#include <vector>
#include "events/trade_flow.hpp"
#include "harness/event_loop.hpp"
#include "pools/twocrypto_fx/twocrypto.hpp"

namespace {
using Tape = arb::events::TradeFlowTape;
using Pool = arb::pools::twocrypto_fx::TwoCryptoPool<double>;
constexpr uint64_t TS = 1'779'753'600;

void require(bool condition, const char* message) {
    if (!condition) { std::cerr << "trade flow failed: " << message << '\n'; std::exit(1); }
}

Pool make_pool() {  // Mid fee 1.46%: prints must clear roughly that band.
    Pool pool({1.0, 1.0}, 50'000.0, 1.1111111111e-8, 0.0146, 0.0170,
              0.054202748, 1e-10, 5e-3, 865.0, 77'235.68, 0.301010101, 0.0);
    pool.set_block_timestamp(TS - 1);
    pool.add_liquidity({43'596'754.65, 564.46165}, 0.0);
    return pool;
}

arb::EventSoA flow_events(const Tape& tape) {
    std::vector<arb::Candle> candles;
    std::vector<uint32_t> bins;
    auto events = arb::EventSoA::from_events(arb::events::gen_trade_flow_events(tape, candles, bins, 0, 0, {}));
    events.flow_bin = bins;
    arb::events::index_trade_flow_events(events, tape);
    return events;
}

// Bins of taker prints at relative moves from the pool price (buys > 0 > sells).
Tape prints(const std::vector<std::pair<double, double>>& moves_and_qty) {
    Tape tape;
    tape.t0 = TS;
    tape.bin_s = 12;
    tape.ptr = {0};
    for (const auto& [move, qty] : moves_and_qty) {
        const double price = 77'235.68 * (1 + move);
        for (auto* column : {&tape.open, &tape.high, &tape.low, &tape.close}) column->push_back(price);
        tape.buy_qty.push_back(move > 0 ? qty : 0);
        tape.sell_qty.push_back(move > 0 ? 0 : qty);
        tape.side.push_back(move > 0 ? 0 : 1);
        tape.bucket.push_back(static_cast<uint32_t>(std::floor(std::log(price) * 1e4)));
        tape.qty.push_back(qty);
        tape.ptr.push_back(tape.qty.size());
    }
    return tape;
}

struct Run { arb::harness::EventLoopResult<double> result; Pool pool; std::vector<arb::harness::Action<double>> actions; };

Run run_maker(const Tape& tape, arb::harness::EventCursor cursor = arb::harness::EventCursor::Scalar) {
    Run run{{}, make_pool(), {}};
    arb::trading::Costs<double> costs; costs.arb_fee_bps = 0.0;
    arb::harness::DonationCfg<double> donation;
    arb::harness::IdleTickCfg<double> idle; idle.freq_s = 0;
    arb::harness::UserSwapCfg<double> user;
    arb::harness::RunConfig<double> config;
    config.min_swap_frac = 1e-8; config.trade_flow = &tape; config.event_cursor = cursor;
    run.result = arb::harness::run_event_loop(
        run.pool, flow_events(tape), costs, donation, idle, user, config, nullptr, 0, &run.actions);
    return run;
}

void npz_bins_become_a_causal_bin_end_clock() {
    const auto path = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "trade-flow-v1.npz";
    const auto tape = arb::events::load_trade_flow(path.string());
    const uint64_t t0 = 1'700'000'004;
    std::vector<arb::Candle> candles;
    std::vector<uint32_t> bins;
    const auto events = arb::events::gen_trade_flow_events(tape, candles, bins, 0, 0, {});
    require(events.size() == 3 && events[0].ts == t0 + 12 && events[2].ts == t0 + 36 &&
            events[0].p_cex == 100.0 && events[1].p_cex == 100.0 && events[2].p_cex == 99.9 &&
            candles[1].volume == 0.0 && bins == std::vector<uint32_t>{0, 1, 2},
            "bin-end clock, carried mark or trace candles changed");
    arb::trading::CexDepthSnapshot fills;
    arb::events::trade_flow_book(tape, 0, fills);
    const double b0 = 46051;
    require(fills.bids.size() == 2 && fills.asks.size() == 1 &&
            fills.bids[0].price == std::exp((b0 + 2) * 1e-4) && fills.bids[0].quantity == 0.5 &&
            fills.bids[1].price == std::exp(b0 * 1e-4) && fills.bids[1].quantity == 1.5 &&
            fills.asks[0].price == std::exp((b0 - 2) * 1e-4) && fills.asks[0].quantity == 2.0,
            "fill book is not taker buys (lower edge, best first) and sells (upper edge)");
    // An excluded first bin neither emits an event nor marks the next, empty one.
    candles.clear(), bins.clear();
    const auto gapped = arb::events::gen_trade_flow_events(tape, candles, bins, 0, 0, {{t0, t0 + 6}});
    require(gapped.size() == 1 && bins == std::vector<uint32_t>{2}, "excluded prints leaked into the mark");
    // Candles: low-first path 100 -> 99.5 -> 101 -> 100.5, volume per 1bp crossed.
    const std::vector<arb::Candle> ohlc{{60, 100, 101, 99.5, 100.5, 12}, {120, 100, 100, 100, 100, 1}, {240, 100, 100, 99, 99, 1}};
    const auto b = [](double price) { return static_cast<uint32_t>(std::floor(std::log(price) * 1e4)); };
    const double legs = (b(100) - b(99.5) + 1) + (b(101) - b(99.5) + 1) + (b(101) - b(100.5) + 1);
    for (const bool volume : {true, false}) {
        const auto approx = arb::events::trade_flow_from_candles(ohlc, volume);
        arb::events::trade_flow_book(approx, 0, fills);
        require(approx.bin_s == 60 && approx.bins() == 4 && std::isnan(approx.close[2]) &&
                approx.ptr[2] == approx.ptr[1] && fills.bids.front().price == std::exp(b(101) * 1e-4) &&
                fills.asks.front().price == std::exp((b(99.5) + 1) * 1e-4),
                "candle path lost its clock, extremes or flat-candle silence");
        require(volume ? std::abs(approx.buy_qty[0] - 12 * (b(101) - b(99.5) + 1) / legs) < 1e-9
                       : fills.bids.size() == 1 && fills.asks.size() == 1 && fills.bids[0].quantity > 1e20,
                "candle volume was not spread over the path, or unlimited extremes were bounded");
    }
}

void maker_fills_are_bounded_by_prints_through_the_pool_quote() {
    require(run_maker(prints({{0.005, 1.0}})).result.metrics.trades == 0,
            "prints inside the pool fee band were hedged");
    for (const double qty : {0.01, 1'000.0}) {
        const auto run = run_maker(prints({{0.05, qty}}));
        require(run.result.metrics.trades == 1 && run.actions.size() == 1, "prints through the quote were not hedged");
        const auto& fill = std::get<arb::harness::ExchangeAction<double>>(run.actions.front());
        require(fill.i == 0 && fill.profit_coin0 > 0, "maker did not buy base from the pool at a profit");
        // Thin prints cap the fill (to 0.2% sizing resolution); deep prints leave it to the pool.
        require(qty < 1 ? fill.dy_after_fee <= qty && fill.dy_after_fee > 0.995 * qty : fill.dy_after_fee < 0.9 * qty,
                "fill size ignored the trade-through volume");
    }
    const auto buy_side = run_maker(prints({{-0.05, 0.01}}));
    const auto& fill = std::get<arb::harness::ExchangeAction<double>>(buy_side.actions.front());
    require(fill.i == 1 && fill.dx <= 0.01 && fill.dx > 0.995 * 0.01, "taker sells did not fill a maker bid");
}

void maker_fast_skip_matches_scalar() {
    std::vector<std::pair<double, double>> bins;
    uint64_t state = 7;
    for (int bin = 0; bin < 240; ++bin) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        const double move = (static_cast<double>(state >> 40) / double(1ULL << 24) - 0.5) * 0.06;
        bins.push_back({move == 0 ? 1e-4 : move, 0.2 + static_cast<double>(state % 1000) / 200});
    }
    const auto tape = prints(bins);
    const auto scalar = run_maker(tape);
    const auto skipped = run_maker(tape, arb::harness::EventCursor::FastSkip);
    require(scalar.result.metrics.trades > 1 && scalar.result.metrics.trades == skipped.result.metrics.trades &&
            scalar.pool.balances == skipped.pool.balances && scalar.pool.cached_price_scale == skipped.pool.cached_price_scale,
            "maker fast skip diverged from the scalar cursor");
}
} // namespace

int main() {
    npz_bins_become_a_causal_bin_end_clock();
    maker_fills_are_bounded_by_prints_through_the_pool_quote();
    maker_fast_skip_matches_scalar();
    std::cout << "test_trade_flow: PASSED\n";
}
