#pragma once

#include <algorithm>
#include <cmath>
#include <limits>

namespace arb::harness {

// Research: a shared rate oracle that a report delivery reads, with a cached value and its write time. Other writers'
// cache comes from the block tape; this run's own writes are kept here. A delivery reads the newer cache unless it is
// older than the age limit (calm_s while the cached rate is within `band` of 1, off_s otherwise); then it reads the
// live value and stores it, which commit() records once a report is actually delivered. Another writer's cache
// stamped in the same block as our write counts as later in that block, so ours stays the cache (a second write in
// a block returns the first one's value).
struct OracleRate {
    double calm_s{0}, off_s{0}, band{0};
    double own_rate{0}, own_ts{-std::numeric_limits<double>::infinity()};

    struct Preview {
        double rate;
        bool refresh;
    };
    Preview preview(double ts, double live, double other_rate, double other_ts) const {
        const bool own = own_ts >= other_ts;
        const double rate = own ? own_rate : other_rate, written = own ? own_ts : other_ts;
        const double limit = std::abs(rate - 1.0) <= band ? calm_s : off_s;
        return ts - written > limit ? Preview{live, true} : Preview{rate, false};
    }
    void commit(double ts, double live) { own_rate = live, own_ts = ts; }
    // price_w() by a transaction of this run: writes the live value unless the cache was already written in this
    // block, where the first write stands (AggregateStablePrice2). Returns whether it wrote.
    bool write(double ts, double live, double other_ts) {
        if (std::max(own_ts, other_ts) >= ts) return false;
        commit(ts, live);
        return true;
    }
};

} // namespace arb::harness
