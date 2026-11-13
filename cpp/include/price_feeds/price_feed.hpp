// Generic price-feed CSV loading and event annotation.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "events/types.hpp"

namespace arb {
namespace price_feeds {

struct PriceFeedPoint {
    double ts{0}; // Unix seconds, preserving fractional report times.
    double price{0.0};
};

// Research: a report bank an NPZ may describe (harness/report_bank.hpp): its average period ("bank_period_s", a scalar;
// 0 when absent) and a rate series read at each delivery ("rate_ts", "rate").
struct ReportBankSpec {
    double period_s{0};
    std::vector<double> rate_ts, rate;
};

std::vector<PriceFeedPoint> load_price_feed_npz(const std::string& path, ReportBankSpec* bank = nullptr);

std::vector<PriceFeedPoint> load_price_feed_csv(const std::string& path, ReportBankSpec* bank = nullptr);

// Fill Event::p_price_feed with the latest feed value at or before ev.ts.
// Events and points are expected to be timestamp-sorted; points may be unsorted
// on input because load_price_feed_csv sorts them.
void attach_price_feed(
    std::vector<Event>& events,
    const std::vector<PriceFeedPoint>& points
);

} // namespace price_feeds
} // namespace arb
