// Idle tick (EMA update) when no trade occurs
#pragma once

#include <cstdint>
#include <utility>
#include "core/common.hpp"
#include "harness/metrics.hpp"

namespace arb {
namespace harness {

// Idle tick configuration
template <typename T>
struct IdleTickCfg {
    uint64_t freq_s{3600};  // seconds between idle ticks (0 = disabled)

    bool randomize{false};  // deterministic uniform interval in [60, freq_s]

    static constexpr uint64_t RANDOM_MIN_INTERVAL_S = 60;

    // Stateless SplitMix64 keeps the schedule reproducible across processes,
    // threads, and optimizer workers. Keying the draw by the prior policy
    // touch preserves fixed-cadence semantics: every successful trade/tick
    // starts one new idle interval, without any mutable RNG state.
    static uint64_t splitmix64(uint64_t x) {
        x += 0x9e3779b97f4a7c15ULL;
        x = (x ^ (x >> 30U)) * 0xbf58476d1ce4e5b9ULL;
        x = (x ^ (x >> 27U)) * 0x94d049bb133111ebULL;
        return x ^ (x >> 31U);
    }

    static uint64_t uniform_bounded(uint64_t seed, uint64_t bound) {
        // Rejection makes the bounded draw exactly uniform rather than using
        // a slightly biased raw modulo. SplitMix advances deterministically
        // if the first 64-bit draw falls in the rejection tail.
        const uint64_t threshold = uint64_t(0) - bound;
        const uint64_t reject_below = threshold % bound;
        uint64_t draw = seed;
        do {
            draw = splitmix64(draw);
        } while (draw < reject_below);
        return draw % bound;
    }

    uint64_t interval_after(uint64_t prior_touch_ts) const {
        if (!randomize || freq_s < RANDOM_MIN_INTERVAL_S) return freq_s;
        const uint64_t width = freq_s - RANDOM_MIN_INTERVAL_S + 1;
        const uint64_t key = prior_touch_ts ^
            (freq_s * 0xd6e8feb86659fd93ULL) ^
            0x4455535452414e44ULL;  // "DUSTRAND"
        return RANDOM_MIN_INTERVAL_S + uniform_bounded(key, width);
    }

    bool due(uint64_t prior_touch_ts, uint64_t event_ts) const {
        return enabled() && event_ts >= prior_touch_ts &&
            event_ts - prior_touch_ts >= interval_after(prior_touch_ts);
    }

    bool enabled() const { return freq_s > 0; }
};

// Run a tick transactionally and commit only an exact price-scale change.
template <typename Pool>
bool try_commit_gated_tick(Pool& pool) {
    Pool candidate = pool;
    try {
        candidate.tick();
    } catch (...) {
        return false;
    }
    if (candidate.cached_price_scale == pool.cached_price_scale) {
        return false;
    }
    pool = std::move(candidate);
    return true;
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

} // namespace harness
} // namespace arb
