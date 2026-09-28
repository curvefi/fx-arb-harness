// Binned CEX taker trades, measured or approximated from candles: a bin-end
// event clock and the trade-through fills of a maker quoting the pool.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "events/time_ranges.hpp"
#include "events/types.hpp"
#include "trading/cex_depth.hpp"

namespace arb::events {

// Taker volume per bin by side and 1bp log-price bucket, packed by
// data/market/pack_trade_flow.py. Bin i spans [t0 + i*bin_s, t0 + (i+1)*bin_s);
// its rows ptr[i]..ptr[i+1] are sorted by side (0 taker buy, 1 taker sell),
// then bucket = floor(1e4 * ln(price)). Prices are NaN in bins without trades.
struct TradeFlowTape {
    uint64_t t0{};
    uint64_t bin_s{};
    std::vector<double> open, high, low, close, buy_qty, sell_qty;
    std::vector<uint64_t> ptr;
    std::vector<uint8_t> side;
    std::vector<uint32_t> bucket;
    std::vector<double> qty;

    size_t bins() const { return close.size(); }
};

TradeFlowTape load_trade_flow(const std::string& path);

// Approximate a tape from OHLCV candles on a regular clock (bin = candle
// interval, first bin at the first candle). Each candle's taker flow follows
// its shorter OHLC path (open, low, high, close or open, high, low, close;
// ties go high first): rising legs print taker buys, falling legs taker sells.
// With volume, the candle volume is spread evenly over every 1bp bucket the
// path crosses; without it, the highest buy and lowest sell print carry
// unlimited volume, so fills are limited only by the pool.
TradeFlowTape trade_flow_from_candles(const std::vector<Candle>& candles, bool volume);

// Fills that resting maker quotes would have received during one bin. Price
// priority fills a resting ask before any taker buy prints above it, so the
// bin's taker buys become the levels the maker can sell base into, and its
// taker sells the levels it can buy base from. A print in bucket b is valued
// at the bucket edge on the maker's side, exp(b/1e4) for buys and
// exp((b+1)/1e4) for sells, which never exceeds what the print allowed.
void trade_flow_book(const TradeFlowTape& tape, size_t bin, trading::CexDepthSnapshot& out);

// One event per bin end. The mark is the last traded price, carried across
// empty bins; bins overlapping an exclusion neither emit events nor move the
// mark. Trace candles keep the bin's OHLCV, and bins[k] is event k's bin.
std::vector<Event> gen_trade_flow_events(
    const TradeFlowTape& tape, std::vector<Candle>& candles, std::vector<uint32_t>& bins,
    uint64_t start_ts, uint64_t end_ts, const TimeRanges& excluded);

// Record each event's fill-book best bid/ask (zero for an empty side) for the
// maker gate and its fast cursor; events.flow_bin must name each event's bin.
void index_trade_flow_events(EventSoA& events, const TradeFlowTape& tape);


} // namespace arb::events
