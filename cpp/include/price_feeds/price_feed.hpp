// Generic price-feed CSV loading and event annotation.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "events/types.hpp"

namespace arb {
namespace price_feeds {

struct PriceFeedPoint {
    uint64_t ts{0};
    double price{0.0};
};

std::vector<PriceFeedPoint> load_price_feed_csv(const std::string& path);

// Fill Event::p_price_feed with the latest feed value at or before ev.ts.
// Events and points are expected to be timestamp-sorted; points may be unsorted
// on input because load_price_feed_csv sorts them.
void attach_price_feed(
    std::vector<Event>& events,
    const std::vector<PriceFeedPoint>& points
);

} // namespace price_feeds
} // namespace arb
