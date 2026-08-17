// Chainlink oracle CSV loading and event annotation.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "events/types.hpp"

namespace arb {
namespace oracles {

namespace detail {

// Exposed only for the byte-equivalence regression test of the hot CSV
// scanner. Production callers should use load_chainlink_csv().
std::vector<std::string> split_csv_line(const std::string& line);

} // namespace detail

struct ChainlinkPoint {
    uint64_t ts{0};
    double price{0.0};
};

std::vector<ChainlinkPoint> load_chainlink_csv(const std::string& path);

// Fill Event::p_chainlink with the latest Chainlink answer at or before ev.ts.
// Events and points are expected to be timestamp-sorted; points may be unsorted
// on input because load_chainlink_csv sorts them.
void attach_chainlink_prices(
    std::vector<Event>& events,
    const std::vector<ChainlinkPoint>& points
);

} // namespace oracles
} // namespace arb
