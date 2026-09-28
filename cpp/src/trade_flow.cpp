// Trade-flow NPZ loading, bin-end events and trade-through fill books.
#include "events/trade_flow.hpp"
#include "io/numeric_npz.hpp"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <stdexcept>

namespace arb::events {
namespace {

[[noreturn]] void invalid(const char* what) {
    throw std::runtime_error(std::string("Invalid trade-flow NPZ: ") + what);
}

template <typename Out>
std::vector<Out> column(const io::Array& array, const char* dtype, size_t count) {
    if (array.dtype != dtype || array.shape != std::vector<size_t>{count}) invalid(dtype);
    std::vector<Out> values(count);
    std::memcpy(values.data(), array.data(), count * sizeof(Out));
    return values;
}

uint64_t scalar(const io::Array& array, const char* dtype, size_t width) {
    if (array.dtype != dtype || !array.shape.empty()) invalid("scalar");
    return io::little(array.data(), width);
}

} // namespace

TradeFlowTape load_trade_flow(const std::string& path) {
    static_assert(sizeof(double) == 8 && sizeof(uint64_t) == 8 && sizeof(uint32_t) == 4);
    if (std::filesystem::path(path).extension() != ".npz")
        throw std::invalid_argument("trade flow requires NPZ; pack it with data/market/pack_trade_flow.py");
    auto arrays = io::read_arrays(path, {"format_version.npy", "bin_s.npy", "t0.npy", "open.npy",
        "high.npy", "low.npy", "close.npy", "buy_qty.npy", "sell_qty.npy", "ptr.npy", "side.npy",
        "bucket.npy", "qty.npy"});
    if (arrays.size() != 13) invalid("missing arrays");
    if (scalar(arrays.at("format_version.npy"), "<u4", 4) != 1) invalid("format_version");
    TradeFlowTape tape;
    tape.bin_s = scalar(arrays.at("bin_s.npy"), "<i8", 8);
    tape.t0 = scalar(arrays.at("t0.npy"), "<i8", 8);
    if (!tape.bin_s || tape.bin_s > 86400 || !tape.t0 || tape.t0 > (uint64_t(1) << 40)) invalid("clock");
    const auto& close = arrays.at("close.npy");
    if (close.shape.size() != 1) invalid("close");
    const size_t bins = close.shape[0];
    tape.open = column<double>(arrays.at("open.npy"), "<f8", bins);
    tape.high = column<double>(arrays.at("high.npy"), "<f8", bins);
    tape.low = column<double>(arrays.at("low.npy"), "<f8", bins);
    tape.close = column<double>(close, "<f8", bins);
    tape.buy_qty = column<double>(arrays.at("buy_qty.npy"), "<f8", bins);
    tape.sell_qty = column<double>(arrays.at("sell_qty.npy"), "<f8", bins);
    tape.ptr = column<uint64_t>(arrays.at("ptr.npy"), "<i8", bins + 1);
    const auto& qty = arrays.at("qty.npy");
    if (qty.shape.size() != 1) invalid("qty");
    const size_t rows = qty.shape[0];
    tape.side = column<uint8_t>(arrays.at("side.npy"), "|u1", rows);
    tape.bucket = column<uint32_t>(arrays.at("bucket.npy"), "<u4", rows);
    tape.qty = column<double>(qty, "<f8", rows);
    if (tape.ptr.front() != 0 || tape.ptr.back() != rows) invalid("ptr");
    for (size_t bin = 0; bin < bins; ++bin) {
        const uint64_t begin = tape.ptr[bin], end = tape.ptr[bin + 1];
        if (end < begin || end > rows) invalid("ptr");
        const bool has = end > begin;
        for (const double price : {tape.open[bin], tape.high[bin], tape.low[bin], tape.close[bin]})
            if (has ? !(std::isfinite(price) && price > 0) : !std::isnan(price)) invalid("prices");
        for (uint64_t row = begin; row < end; ++row) {
            if (tape.side[row] > 1 || !tape.bucket[row] || !(std::isfinite(tape.qty[row]) && tape.qty[row] > 0))
                invalid("profile row");
            if (row > begin && (tape.side[row] < tape.side[row - 1] ||
                    (tape.side[row] == tape.side[row - 1] && tape.bucket[row] <= tape.bucket[row - 1])))
                invalid("profile order");
        }
    }
    return tape;
}

void trade_flow_book(const TradeFlowTape& tape, size_t bin, trading::CexDepthSnapshot& out) {
    out.available_ns = (tape.t0 + (bin + 1) * tape.bin_s) * 1'000'000'000ULL;
    out.bids.clear();
    out.asks.clear();
    const uint64_t begin = tape.ptr[bin], end = tape.ptr[bin + 1];
    uint64_t split = begin;
    while (split < end && tape.side[split] == 0) ++split;
    // Taker buys, best (highest) first; taker sells, best (lowest) first.
    for (uint64_t row = split; row-- > begin;)
        out.bids.push_back({std::exp(static_cast<double>(tape.bucket[row]) * 1e-4), tape.qty[row]});
    for (uint64_t row = split; row < end; ++row)
        out.asks.push_back({std::exp(static_cast<double>(tape.bucket[row] + 1) * 1e-4), tape.qty[row]});
}

std::vector<Event> gen_trade_flow_events(
    const TradeFlowTape& tape, std::vector<Candle>& candles, std::vector<uint32_t>& bins,
    uint64_t start_ts, uint64_t end_ts, const TimeRanges& excluded) {
    validate_time_ranges(excluded);
    std::vector<Event> events;
    double mark = std::numeric_limits<double>::quiet_NaN();
    for (size_t bin = 0; bin < tape.bins(); ++bin) {
        const uint64_t begin = tape.t0 + bin * tape.bin_s, ts = begin + tape.bin_s;
        bool overlaps = time_excluded(ts, excluded);
        for (const auto& range : excluded) overlaps = overlaps || (begin < range[1] && ts > range[0]);
        if (overlaps) continue;
        const bool has = std::isfinite(tape.close[bin]);  // A flat candle prices without prints.
        if (has) mark = tape.close[bin];
        if (ts < start_ts || (end_ts && ts > end_ts) || !(mark > 0)) continue;
        if (candles.size() > std::numeric_limits<uint32_t>::max())
            throw std::invalid_argument("Trade-flow event indices exceed uint32_t");
        const double volume = tape.buy_qty[bin] + tape.sell_qty[bin];
        const auto index = static_cast<uint32_t>(candles.size());
        candles.push_back(has ? Candle{begin, tape.open[bin], tape.high[bin], tape.low[bin], mark, volume}
                              : Candle{begin, mark, mark, mark, mark, 0.0});
        events.push_back({ts, mark, 0.0, 0, volume, index});
        bins.push_back(static_cast<uint32_t>(bin));
    }
    return events;
}

void index_trade_flow_events(EventSoA& events, const TradeFlowTape& tape) {
    if (events.flow_bin.size() != events.size())
        throw std::invalid_argument("trade-flow events require one bin per event");
    events.fill_bid.assign(events.size(), 0.0);
    events.fill_ask.assign(events.size(), 0.0);
    trading::CexDepthSnapshot fills;
    for (size_t index = 0; index < events.size(); ++index) {
        trade_flow_book(tape, events.flow_bin[index], fills);
        if (!fills.bids.empty()) events.fill_bid[index] = fills.bids.front().price;
        if (!fills.asks.empty()) events.fill_ask[index] = fills.asks.front().price;
    }
}

TradeFlowTape trade_flow_from_candles(const std::vector<Candle>& candles, bool volume) {
    if (candles.empty()) throw std::invalid_argument("trade flow needs candles");
    TradeFlowTape tape;
    tape.t0 = candles.front().ts;
    for (size_t i = 1; i < candles.size(); ++i) {
        const uint64_t step = candles[i].ts - candles[i - 1].ts;
        if (candles[i].ts <= candles[i - 1].ts) throw std::invalid_argument("candle timestamps must increase");
        tape.bin_s = tape.bin_s ? std::min(tape.bin_s, step) : step;
    }
    if (!tape.bin_s) tape.bin_s = 60;
    constexpr double UNLIMITED = 1e30;  // Capped by pool balances, never by prints.
    const auto bucket = [](double price) { return static_cast<uint32_t>(std::floor(std::log(price) * 1e4)); };
    tape.ptr = {0};
    size_t next = 0;
    const size_t bins = (candles.back().ts - tape.t0) / tape.bin_s + 1;
    for (size_t bin = 0; bin < bins; ++bin) {
        const uint64_t begin = tape.t0 + bin * tape.bin_s;
        if (next < candles.size() && candles[next].ts != begin && (candles[next].ts - tape.t0) % tape.bin_s)
            throw std::invalid_argument("candles are not on one regular clock");
        const Candle* c = next < candles.size() && candles[next].ts == begin ? &candles[next++] : nullptr;
        const double nan = std::numeric_limits<double>::quiet_NaN();
        tape.open.push_back(c ? c->open : nan);
        tape.high.push_back(c ? c->high : nan);
        tape.low.push_back(c ? c->low : nan);
        tape.close.push_back(c ? c->close : nan);
        // Taker volume per bucket by side: [0] buys, [1] sells.
        std::map<uint32_t, double> side[2];
        double buy_total = 0, sell_total = 0;
        if (c) {
            const bool low_first = std::abs(c->open - c->low) + std::abs(c->high - c->close) <
                                   std::abs(c->open - c->high) + std::abs(c->low - c->close);
            const double path[4] = {c->open, low_first ? c->low : c->high, low_first ? c->high : c->low, c->close};
            size_t crossed = 0;
            for (int leg = 0; leg < 3; ++leg)
                if (path[leg] != path[leg + 1])
                    crossed += std::abs(int64_t(bucket(path[leg + 1])) - int64_t(bucket(path[leg]))) + 1;
            for (int leg = 0; leg < 3 && crossed; ++leg) {
                if (path[leg] == path[leg + 1]) continue;
                const bool up = path[leg + 1] > path[leg];
                const uint32_t from = bucket(path[leg]), to = bucket(path[leg + 1]);
                auto& levels = side[up ? 0 : 1];
                if (!volume) {
                    levels[to] = UNLIMITED;
                    continue;
                }
                const double share = c->volume / static_cast<double>(crossed);
                for (uint32_t b = std::min(from, to); b <= std::max(from, to); ++b) levels[b] += share;
            }
            if (!volume) {
                // Only the extreme prints matter when volume is unlimited.
                if (!side[0].empty()) side[0] = {*side[0].rbegin()};
                if (!side[1].empty()) side[1] = {*side[1].begin()};
            }
        }
        for (int s = 0; s < 2; ++s)
            for (const auto& [b, qty] : side[s]) {
                if (!(qty > 0)) continue;
                tape.side.push_back(static_cast<uint8_t>(s));
                tape.bucket.push_back(b);
                tape.qty.push_back(qty);
                (s == 0 ? buy_total : sell_total) += qty;
            }
        tape.buy_qty.push_back(volume ? buy_total : (c ? c->volume : 0.0) / 2);
        tape.sell_qty.push_back(volume ? sell_total : (c ? c->volume : 0.0) / 2);
        tape.ptr.push_back(tape.qty.size());
    }
    return tape;
}

} // namespace arb::events
