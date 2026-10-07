// Per-block CEX tape: prices and traded volume at fixed offsets after each
// block timestamp, packed by data/market/build_block_tape.py.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "events/time_ranges.hpp"
#include "events/types.hpp"

namespace arb::events {

inline constexpr size_t BLOCK_PRICE_OFFSETS = 5;   // 0..4 s
inline constexpr size_t BLOCK_VOLUME_OFFSETS = 3;  // 0..2 s

// Row k describes block B = t0 + k * block_s. price[k][o] is the last trade
// strictly before B + o (NaN before the first trade); volume[k][o] is the base
// volume traded in [B + o - block_s, B + o).
struct BlockTape {
    uint64_t t0{};
    uint64_t block_s{};
    std::vector<std::array<double, BLOCK_PRICE_OFFSETS>> price;
    std::vector<std::array<double, BLOCK_VOLUME_OFFSETS>> volume;

    size_t blocks() const { return price.size(); }
};

BlockTape load_block_tape(const std::string& path);

// One event per block B in [start_ts, end_ts] (end 0: open) whose tape row
// window [B - block_s, B + 4) avoids every exclusion and whose price at
// B + delay_s is known. The event is timestamped B (the pool's clock) and
// carries the CEX state at B + delay_s, when the arbitrageur's information ends:
// price P(B + delay_s) and volume V(B + delay_s). Each event gets a synthetic
// candle (open P(B), high/low over the row's prices, close the event price).
EventSoA gen_block_events(
    const BlockTape& tape, size_t delay_s, std::vector<Candle>& candles,
    uint64_t start_ts, uint64_t end_ts, const TimeRanges& excluded);

} // namespace arb::events
