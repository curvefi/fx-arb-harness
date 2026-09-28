// Research valuation: externally marked LP collateral less projected debt, in coin1.
// This is not the legacy nonlinear YB oracle metric. Sampling is hourly + endpoint.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
namespace arb::harness {
struct YbExternalEquity {
    bool initialized{false};
    double initial{0}, equity{-1}, growth{-1}, peak{0}, max_drawdown{0};
    template<class Pool, class Actor, class Price>
    void sample(const Pool& pool, const Actor& actor, uint64_t ts, Price price) {
        const double px = static_cast<double>(price);
        const double supply = static_cast<double>(pool.totalSupply);
        if (!(px > 0) || !(supply > 0))
            throw std::runtime_error("external YB mark requires positive price and LP supply");
        const double lp = (static_cast<double>(pool.balances[0]) + px * static_cast<double>(pool.balances[1])) / supply;
        equity = (static_cast<double>(actor.state().collateral) * lp - static_cast<double>(actor.projected_debt(ts))) / px;
        // Preserve the fresh-seed arithmetic exactly when pending LT cash is zero.
        const double pending_cash = static_cast<double>(actor.state().lt_stable_balance);
        if (pending_cash != 0) equity += pending_cash / px;
        if (!std::isfinite(equity)) throw std::runtime_error("nonfinite external YB equity");
        if (!initialized) {
            if (!(equity > 0)) throw std::runtime_error("nonpositive initial external YB equity");
            initialized = true; initial = peak = equity;
        }
        growth = equity / initial; // Retain negative equity; do not skip insolvency.
        peak = std::max(peak, equity);
        max_drawdown = std::max(max_drawdown, 1.0 - equity / peak);
    }
};
} // namespace arb::harness
