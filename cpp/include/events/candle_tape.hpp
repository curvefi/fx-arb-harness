// OHLCV candles on one regular clock: a bin-end event clock.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "events/time_ranges.hpp"
#include "events/types.hpp"

namespace arb::events {

// OHLC and traded base volume per bin.
// Bin i spans [t0 + i*bin_s, t0 + (i+1)*bin_s). Prices are NaN in bins without a candle.
struct CandleTape {
    uint64_t t0{};
    uint64_t bin_s{};
    std::vector<double> open, high, low, close, volume;

    size_t bins() const { return close.size(); }
};

// A tape from OHLCV candles on a regular clock (bin = candle interval, first bin at the first candle).
CandleTape make_candle_tape(const std::vector<Candle>& candles);

// One event per bin end. The mark is the last close, carried across
// empty bins; bins overlapping an exclusion neither emit events nor move the
// mark. Trace candles keep the bin's OHLCV.
std::vector<Event> gen_candle_events(
    const CandleTape& tape, std::vector<Candle>& candles,
    uint64_t start_ts, uint64_t end_ts, const TimeRanges& excluded);

} // namespace arb::events
