// Idle tick (EMA update) when no trade occurs
#pragma once

#include <cstdint>
#include "core/common.hpp"
#include "harness/metrics.hpp"

namespace arb {
namespace harness {

// Idle tick configuration
template <typename T>
struct IdleTickCfg {
    uint64_t freq_s{3600};  // seconds between idle ticks (0 = disabled)

    bool due(uint64_t prior_touch_ts, uint64_t event_ts) const {
        return enabled() && event_ts >= prior_touch_ts &&
            event_ts - prior_touch_ts >= freq_s;
    }

    bool enabled() const { return freq_s > 0; }
};

// Poke the pool (tweak_price and the policy callback, no tokens move) and count a price_scale move.
// Returns true if the tick committed.
template <typename T, typename Pool>
bool tick_pool(Pool& pool, Metrics<T>& m) {
    const T ps_before = pool.cached_price_scale;

    try {
        pool.tick();

        const T ps_after = pool.cached_price_scale;
        if (differs_rel(ps_after, ps_before)) {
            m.n_rebalances += 1;
        }
        return true;

    } catch (...) {
        // Ignore failed tick
        return false;
    }
}

// Try to perform an idle tick (EMA/oracle update) if enough time has passed.
// Called when no arb trade was executed for this event.
// Returns true if tick was performed.
template <typename T, typename Pool>
bool try_idle_tick(
    Pool& pool,
    const IdleTickCfg<T>& cfg,
    uint64_t ev_ts,
    Metrics<T>& m
) {
    if (!cfg.enabled()) return false;

    // Check if enough time has passed since last pool update
    if (!cfg.due(pool.last_timestamp, ev_ts)) return false;

    return tick_pool(pool, m);
}

// A report keeper's touch (deliver_oracle_report and a dust swap in one transaction): it needs only the block's first
// touch (dt > 0), not freq_s of pool idle time, so a reportless touch shortly before cannot cancel a due report.
template <typename T, typename Pool>
bool try_keeper_tick(Pool& pool, uint64_t ev_ts, Metrics<T>& m) {
    if (!(pool.last_timestamp < ev_ts)) return false;
    return tick_pool(pool, m);
}

} // namespace harness
} // namespace arb
