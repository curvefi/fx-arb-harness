// Events module - loading and generation functions
#pragma once

#include "events/types.hpp"
#include <string>
#include <vector>

namespace arb {
namespace events { class CexDepthTape; }

// Load candles from JSON file.
// Format: array of [ts, open, high, low, close, volume]
// squeeze_frac: clamp high/low to ±squeeze_frac around (open+close)/2
std::vector<Candle> load_candles(const std::string& path,
                                  size_t max_candles = 0,
                                  double squeeze_frac = 0.999,
                                  uint64_t start_ts = 0);

// Generate two price events per candle (low-first or high-first path).
std::vector<Event> gen_events(const std::vector<Candle>& candles);

// Use regular depth observations while a published book is fresh, and the
// existing candle path otherwise. Appends flat trace candles for depth events.
std::vector<Event> gen_mixed_depth_events(
    std::vector<Candle>& candles, const events::CexDepthTape& depth,
    uint64_t observation_interval_s, uint64_t max_age_s, uint64_t start_ts = 0);

} // namespace arb
