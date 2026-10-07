// Block-tape NPZ loading and per-block events.
#include "events/block_tape.hpp"
#include "io/numeric_npz.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <stdexcept>

namespace arb::events {
namespace {

[[noreturn]] void invalid(const char* what) {
    throw std::runtime_error(std::string("Invalid block NPZ: ") + what);
}

uint64_t scalar(const io::Array& array, const char* dtype, size_t width) {
    if (array.dtype != dtype || !array.shape.empty()) invalid("scalar");
    return io::little(array.data(), width);
}

template <size_t N>
std::vector<std::array<double, N>> rows(const io::Array& array, size_t count) {
    if (array.dtype != "<f8" || array.shape != std::vector<size_t>{count, N}) invalid("matrix");
    std::vector<std::array<double, N>> out(count);
    std::memcpy(out.data(), array.data(), count * N * sizeof(double));
    return out;
}

void offsets(const io::Array& array, size_t count) {
    if (array.dtype != "<i8" || array.shape != std::vector<size_t>{count}) invalid("offsets");
    for (size_t o = 0; o < count; ++o)
        if (io::little(array.data() + 8 * o, 8) != o) invalid("offsets");
}

} // namespace

BlockTape load_block_tape(const std::string& path) {
    static_assert(sizeof(std::array<double, BLOCK_PRICE_OFFSETS>) == BLOCK_PRICE_OFFSETS * sizeof(double));
    if (std::filesystem::path(path).extension() != ".npz")
        throw std::invalid_argument("block tape requires NPZ; build it with data/market/build_block_tape.py");
    auto arrays = io::read_arrays(path, {"format_version.npy", "t0.npy", "block_s.npy",
        "price_offsets_s.npy", "volume_offsets_s.npy", "price.npy", "volume.npy"});
    if (arrays.size() != 7) invalid("missing arrays");
    if (scalar(arrays.at("format_version.npy"), "<u4", 4) != 1) invalid("format_version");
    BlockTape tape;
    tape.t0 = scalar(arrays.at("t0.npy"), "<i8", 8);
    tape.block_s = scalar(arrays.at("block_s.npy"), "<i8", 8);
    if (tape.block_s != 12 || tape.t0 % 12 != 11 || tape.t0 > (uint64_t(1) << 40)) invalid("clock");
    offsets(arrays.at("price_offsets_s.npy"), BLOCK_PRICE_OFFSETS);
    offsets(arrays.at("volume_offsets_s.npy"), BLOCK_VOLUME_OFFSETS);
    const auto& price = arrays.at("price.npy");
    if (price.shape.size() != 2 || price.shape[0] == 0) invalid("price");
    const size_t n = price.shape[0];
    tape.price = rows<BLOCK_PRICE_OFFSETS>(price, n);
    tape.volume = rows<BLOCK_VOLUME_OFFSETS>(arrays.at("volume.npy"), n);
    bool traded = false;
    for (size_t k = 0; k < n; ++k) {
        for (const double p : tape.price[k]) {
            if (std::isnan(p) && !traded) continue;
            if (!(std::isfinite(p) && p > 0)) invalid("price");
            traded = true;
        }
        for (const double v : tape.volume[k])
            if (!(std::isfinite(v) && v >= 0)) invalid("volume");
    }
    return tape;
}

EventSoA gen_block_events(
    const BlockTape& tape, size_t delay_s, std::vector<Candle>& candles,
    uint64_t start_ts, uint64_t end_ts, const TimeRanges& excluded) {
    if (delay_s >= BLOCK_VOLUME_OFFSETS) throw std::invalid_argument("block delay must be 0, 1 or 2 s");
    validate_time_ranges(excluded);
    EventSoA events;
    for (size_t k = 0; k < tape.blocks(); ++k) {
        const uint64_t ts = tape.t0 + k * tape.block_s;
        if (ts < start_ts || (end_ts && ts > end_ts)) continue;
        const uint64_t begin = ts - tape.block_s, end = ts + BLOCK_PRICE_OFFSETS - 1;
        if (std::any_of(excluded.begin(), excluded.end(),
                        [&](const auto& range) { return begin < range[1] && end > range[0]; }))
            continue;
        const auto& price = tape.price[k];
        const double seen = price[delay_s];
        if (!(seen > 0)) continue;
        double high = seen, low = seen;
        for (const double p : price) {
            if (!(p > 0)) continue;
            high = std::max(high, p);
            low = std::min(low, p);
        }
        if (candles.size() > std::numeric_limits<uint32_t>::max())
            throw std::invalid_argument("Block event indices exceed uint32_t");
        const double volume = tape.volume[k][delay_s];
        events.ts.push_back(ts);
        events.p_cex.push_back(seen);
        events.volume.push_back(volume);
        events.candle_idx.push_back(static_cast<uint32_t>(candles.size()));
        candles.push_back({ts, price[0] > 0 ? price[0] : seen, high, low, seen, volume});
    }
    return events;
}

} // namespace arb::events
