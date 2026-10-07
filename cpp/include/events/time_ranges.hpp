#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace arb::events {

// UTC Unix seconds, half-open [start, end). Exclusions preserve calendar time.
using TimeRanges = std::vector<std::array<uint64_t, 2>>;

inline void validate_time_ranges(const TimeRanges& ranges) {
    uint64_t previous_end = 0;
    for (const auto& range : ranges) {
        if (range[0] >= range[1] || range[0] < previous_end)
            throw std::invalid_argument("excluded_time_ranges must be ordered, disjoint [start, end) pairs");
        previous_end = range[1];
    }
}

inline bool time_excluded(uint64_t ts, const TimeRanges& ranges) {
    return std::any_of(ranges.begin(), ranges.end(), [ts](const auto& range) {
        return ts >= range[0] && ts < range[1];
    });
}

} // namespace arb::events
