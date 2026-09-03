// Synthetic user swap simulation
#pragma once

#include <cstdint>
#include <cstddef>
#include <cmath>

#include "pools/twocrypto_fx/helpers.hpp"

namespace arb {
namespace harness {

// User swap configuration
template <typename T>
struct UserSwapCfg {
    uint64_t freq_s{0};           // seconds between user swaps (0 = disabled)
    // Daily fair-TVL utilization. The protocol name remains
    // user_swap_size_frac for wire compatibility.
    T        size_frac{T(0.01)};
    T        thresh{T(0.05)};     // max post-fee execution disadvantage vs CEX

    // Runtime state
    uint64_t next_ts{0};          // next scheduled user swap timestamp
    size_t   next_dir{0};         // 0 = coin0->coin1, 1 = coin1->coin0

    bool enabled() const { return freq_s > 0 && size_frac > T(0); }

    void init(uint64_t start_ts) {
        if (enabled()) {
            next_ts = start_ts + freq_s;
        }
    }
};

// Try to perform a synthetic user swap if due.
// Alternates direction each swap, only executes if fee-inclusive execution is
// no worse than CEX by cfg.thresh.
// Returns true if swap was executed.
template <typename T, typename Pool>
bool try_user_swap(
    Pool& pool,
    UserSwapCfg<T>& cfg,
    uint64_t ev_ts,
    T p_cex
) {
    if (!cfg.enabled()) return false;
    if (cfg.next_ts == 0 || ev_ts < cfg.next_ts) return false;

    // Advance schedule regardless of whether swap succeeds
    cfg.next_ts += cfg.freq_s;

    // Validate CEX price
    if (!(p_cex > T(0))) return false;

    // Determine swap direction and equal coin0-value amount. Scaling the
    // daily utilization by the cadence makes 1.0 mean 100% fair-TVL turnover
    // per day, independent of which input reserve is selected.
    const size_t i_from = cfg.next_dir & 1;
    const size_t j_to = i_from ^ 1;

    const T bal_from = pool.balances[i_from];
    if (!(bal_from > T(0))) return false;

    T daily_tvl_frac = cfg.size_frac;
    if (daily_tvl_frac > T(1)) daily_tvl_frac = T(1);
    if (!(daily_tvl_frac > T(0))) return false;

    constexpr long double SECONDS_PER_DAY = 86400.0L;
    const T fair_tvl = pool.balances[0] + p_cex * pool.balances[1];
    const T cadence_days = static_cast<T>(
        static_cast<long double>(cfg.freq_s) / SECONDS_PER_DAY
    );
    const T notional_coin0 = fair_tvl * daily_tvl_frac * cadence_days;
    const T dx = i_from == 0 ? notional_coin0 : notional_coin0 / p_cex;
    if (!(dx > T(0))) return false;

    auto [dy_after_fee, sim_fee] = pools::twocrypto_fx::simulate_exchange_once(
        pool, i_from, j_to, dx);
    (void)sim_fee;
    if (!(dy_after_fee > T(0))) return false;

    if (i_from == 0) {
        // User buys risky asset with stable coin. Lower effective price is better.
        const T effective_ask = dx / dy_after_fee;
        if (!(effective_ask <= p_cex * (T(1) + cfg.thresh))) return false;
    } else {
        // User sells risky asset for stable coin. Higher effective price is better.
        const T effective_bid = dy_after_fee / dx;
        if (!(effective_bid >= p_cex * (T(1) - cfg.thresh))) return false;
    }

    // The pool SDK makes exchange atomic across trailing tweak failures.
    try {
        (void)pool.exchange(static_cast<T>(i_from), static_cast<T>(j_to), dx, T(0));
        cfg.next_dir ^= 1;  // alternate direction for next swap
        return true;

    } catch (...) {
        return false;
    }
}

} // namespace harness
} // namespace arb
