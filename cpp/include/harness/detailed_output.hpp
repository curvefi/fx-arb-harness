// Detailed per-candle output value types (transport-free).
#pragma once

#include <cstdint>
#include "events/types.hpp"
#include "harness/precision.hpp"

namespace arb {
namespace harness {

// Per-candle state entry matching detailed-output format
template <typename T>
struct DetailedEntry {
    uint64_t t{0};        // candle timestamp
    T token0{};           // pool balance[0]
    T token1{};           // pool balance[1]
    T D{};                // xp-normalized invariant D at trace capture
    T xp_0{};             // xp-normalized balance[0] at trace capture
    T xp_1{};             // xp-normalized balance[1] at trace capture
    T price_oracle{};     // EMA oracle price
    T price_scale{};      // price scale
    T profit{};           // virtual_price - 1.0
    T vp{};               // virtual price
    T vp_boosted{};       // virtual price with donation boost
    T xcp{};              // raw xcp_profit value
    T lp_xcp_profit{};    // LP profit path used by apy_net_gm
    T total_supply{};     // total LP supply
    T donation_apy{};     // annual donation rate used by the harness
    T donation_shares{};  // donation shares balance
    T donation_unlocked{};// unlocked donation shares
    T last_prices{};      // last spot price used for EMA
    uint64_t last_timestamp{0}; // last EMA update timestamp
    T open{};             // candle OHLC
    T high{};
    T low{};
    T close{};
    T p_cex{};            // event price used for this tick
    T p_chainlink{};      // latest Chainlink price available at this tick
    T fee{};              // dynamic fee at this point
    T slippage_1pct_0to1{}; // current 1%-TVL real slippage, coin0 -> coin1
    T slippage_1pct_1to0{}; // current 1%-TVL real slippage, coin1 -> coin0
    uint64_t n_trades{0};  // cumulative trade count
    uint64_t n_rebalances{0}; // cumulative rebalance count
    uint64_t yb_initialized{0}; // whether the online YB tracker has a valid state
    MetricF<T> yb_growth{0}; // online YB deposit growth after this event
    MetricF<T> yb_fee{0};    // YB releverage fee applied at this event
    uint64_t yb_releverage_trades{0}; // cumulative online YB trade count
    T yb_stable_balance{};
    T yb_debt{};
    T yb_collateral_lp{};
    T yb_lp_oracle{};
    T yb_lp_fair{};
};

} // namespace harness
} // namespace arb
