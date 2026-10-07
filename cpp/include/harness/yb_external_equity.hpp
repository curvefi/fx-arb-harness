// Research valuation: externally marked LP collateral less projected debt, in coin1.
// This is not the legacy nonlinear YB oracle metric. Sampling is hourly + endpoint.
// Exposure: u is the coin0 part of the marked equity (LP share of balance 0, less debt, plus pending cash) and
// x = u / (price * equity). exposure_return sums u * (1/price - 1/previous price) between samples, as a fraction
// of the initial equity; exposure_rms is the time-weighted root mean square of x (sample-and-hold).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
namespace arb::harness {
struct YbExternalEquity {
    bool initialized{false};
    double initial{0}, equity{-1}, growth{-1}, peak{0}, max_drawdown{0};
    double exposure_return{0}, exposure_sq_time{0}, sampled_time{0}, last_u{0}, last_px{0}, last_x{0};
    uint64_t last_ts{0};
    double exposure_rms() const { return sampled_time > 0 ? std::sqrt(exposure_sq_time / sampled_time) : -1.0; }
    template<class Pool, class Actor, class Price>
    void sample(const Pool& pool, const Actor& actor, uint64_t ts, Price price) {
        const double px = static_cast<double>(price);
        const double supply = static_cast<double>(pool.totalSupply);
        if (!(px > 0) || !(supply > 0))
            throw std::runtime_error("external YB mark requires positive price and LP supply");
        const double lp = (static_cast<double>(pool.balances[0]) + px * static_cast<double>(pool.balances[1])) / supply;
        const double collateral = static_cast<double>(actor.state().collateral);
        const double debt = static_cast<double>(actor.projected_debt(ts));
        equity = (collateral * lp - debt) / px;
        // Preserve the fresh-seed arithmetic exactly when pending LT cash is zero.
        const double pending_cash = static_cast<double>(actor.state().lt_stable_balance);
        if (pending_cash != 0) equity += pending_cash / px;
        const double u = collateral * static_cast<double>(pool.balances[0]) / supply - debt + pending_cash;
        if (!std::isfinite(equity)) throw std::runtime_error("nonfinite external YB equity");
        if (!initialized) {
            if (!(equity > 0)) throw std::runtime_error("nonpositive initial external YB equity");
            initialized = true; initial = peak = equity;
        } else if (ts > last_ts) {
            exposure_return += last_u * (1.0 / px - 1.0 / last_px) / initial;
            exposure_sq_time += last_x * last_x * static_cast<double>(ts - last_ts);
            sampled_time += static_cast<double>(ts - last_ts);
        }
        last_u = u; last_px = px; last_ts = ts;
        last_x = equity > 0 ? u / (px * equity) : 0.0;
        growth = equity / initial; // Retain negative equity; do not skip insolvency.
        peak = std::max(peak, equity);
        max_drawdown = std::max(max_drawdown, 1.0 - equity / peak);
    }
};
} // namespace arb::harness
