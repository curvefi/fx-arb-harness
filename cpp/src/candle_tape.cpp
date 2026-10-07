// Candle tape and bin-end events.
#include "events/candle_tape.hpp"

#include <cmath>
#include <limits>
#include <algorithm>
#include <stdexcept>

namespace arb::events {

std::vector<Event> gen_candle_events(
    const CandleTape& tape, std::vector<Candle>& candles,
    uint64_t start_ts, uint64_t end_ts, const TimeRanges& excluded) {
    validate_time_ranges(excluded);
    std::vector<Event> events;
    double mark = std::numeric_limits<double>::quiet_NaN();
    for (size_t bin = 0; bin < tape.bins(); ++bin) {
        const uint64_t begin = tape.t0 + bin * tape.bin_s, ts = begin + tape.bin_s;
        bool overlaps = time_excluded(ts, excluded);
        for (const auto& range : excluded) overlaps = overlaps || (begin < range[1] && ts > range[0]);
        if (overlaps) continue;
        const bool has = std::isfinite(tape.close[bin]);  // A flat candle still sets the price.
        if (has) mark = tape.close[bin];
        if (ts < start_ts || (end_ts && ts > end_ts) || !(mark > 0)) continue;
        if (candles.size() > std::numeric_limits<uint32_t>::max())
            throw std::invalid_argument("Candle event indices exceed uint32_t");
        const double volume = tape.volume[bin];
        const auto index = static_cast<uint32_t>(candles.size());
        candles.push_back(has ? Candle{begin, tape.open[bin], tape.high[bin], tape.low[bin], mark, volume}
                              : Candle{begin, mark, mark, mark, mark, 0.0});
        events.push_back({ts, mark, 0.0, 0, volume, index});
    }
    return events;
}

CandleTape make_candle_tape(const std::vector<Candle>& candles) {
    if (candles.empty()) throw std::invalid_argument("a candle tape needs candles");
    CandleTape tape;
    tape.t0 = candles.front().ts;
    for (size_t i = 1; i < candles.size(); ++i) {
        const uint64_t step = candles[i].ts - candles[i - 1].ts;
        if (candles[i].ts <= candles[i - 1].ts) throw std::invalid_argument("candle timestamps must increase");
        tape.bin_s = tape.bin_s ? std::min(tape.bin_s, step) : step;
    }
    if (!tape.bin_s) tape.bin_s = 60;
    size_t next = 0;
    const size_t bins = (candles.back().ts - tape.t0) / tape.bin_s + 1;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    for (size_t bin = 0; bin < bins; ++bin) {
        const uint64_t begin = tape.t0 + bin * tape.bin_s;
        if (next < candles.size() && candles[next].ts != begin && (candles[next].ts - tape.t0) % tape.bin_s)
            throw std::invalid_argument("candles are not on one regular clock");
        const Candle* c = next < candles.size() && candles[next].ts == begin ? &candles[next++] : nullptr;
        tape.open.push_back(c ? c->open : nan);
        tape.high.push_back(c ? c->high : nan);
        tape.low.push_back(c ? c->low : nan);
        tape.close.push_back(c ? c->close : nan);
        tape.volume.push_back(c ? c->volume : 0.0);
    }
    return tape;
}

} // namespace arb::events
