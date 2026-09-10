#pragma once
#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>
#include <utility>
#include "events/observed_state.hpp"
#include "harness/actions.hpp"
#include "harness/yb_reference_2l.hpp"
#include "pools/restore_public.hpp"

namespace arb::harness {
enum class StateReconciliationMode { Off, OnPriceScaleDetach };

template<class Pool> auto public_pool_values(const Pool& p) {
    return std::tie(p.balances, p.admin_balances, p.D, p.totalSupply,
        p.cached_price_scale, p.cached_price_oracle, p.last_prices, p.last_timestamp,
        p.virtual_price, p.xcp_profit, p.lp_xcp_profit, p.last_admin_fee_claim_timestamp,
        p.donation_shares, p.last_donation_release_ts, p.donation_protection_expiry_ts,
        p.donation_protection_period, p.donation_protection_lp_threshold,
        p.donation_protection_extension_remainder, p.donation_shares_max_ratio,
        p.donation_duration, p.A, p.gamma, p.mid_fee, p.out_fee, p.fee_gamma,
        p.adjustment_step_min, p.adjustment_step_max, p.ma_time,
        p.reserved_profit_fraction, p.admin_fee, p.precisions);
}
template<class T> auto public_yb_values(const YbReference2LState<T>& s) {
    return std::tie(s.leverage, s.lev_ratio, s.min_safe_debt_ratio, s.max_safe_debt_ratio,
        s.fee, s.collateral, s.debt, s.rate, s.rate_mul, s.rate_time, s.minted,
        s.redeemed, s.stable_balance, s.flash_max_loan, s.stable_aggregator,
        s.rounding_discount, s.lt_donation_discount, s.lt_stable_balance, s.killed);
}
template<class T> class StateReconciliation {
public:
    StateReconciliation(const events::ObservedStateTape<T>& tape,
        StateReconciliationMode mode, T threshold_bps, uint64_t delay_s)
        : tape_(tape), mode_(mode), threshold_(threshold_bps / T(10000)), delay_ns_(delay_s * NS) {}
    void advance(uint64_t wall) {
        const auto& rows = tape_.rows();
        while (next_ < rows.size() && rows[next_].available_ns <= wall) {
            latest_ = &rows[next_++]; ++summary.observations;
        }
    }
    template<class Log> void check_price_scale_detachment(T scale, uint64_t wall, Log log) {
        if (mode_ == StateReconciliationMode::Off || requested_ || !latest_) return;
        const T production = latest_->pool_init.historical_state.price_scale;
        if (!(production > T(0)) || !std::isfinite(scale) ||
            std::abs(scale-production) < production*threshold_) return;
        requested_ = true; detection_ns_ = wall;
        deadline_ns_ = wall + std::min(delay_ns_, UINT64_MAX-wall);
        ++summary.episodes;
        auto action = record("request", wall);
        action.scale_before = scale; action.scale_after = production;
        log(std::move(action));
    }
    bool draining() const { return requested_; }
    template<class Pool, class Log>
    bool apply_if_ready(uint64_t wall, Pool& pool, YbReference2LMarket<T>& yb, Log log) {
        if (!requested_ || wall < deadline_ns_ || !latest_) return false;
        auto replacement = pools::pool_from_public_state(latest_->pool_init);
        auto replacement_yb = yb;
        replacement_yb.restore_public_state(latest_->yb_initial_state, wall / NS);
        const bool equal = public_pool_values(pool) == public_pool_values(replacement) &&
                           public_yb_values(yb.state()) == public_yb_values(replacement_yb.state());
        auto action = record(equal ? "equal" : "apply", wall); action.apply_ns = wall;
        action.state_changed = !equal;
        action.balances_before = pool.balances; action.balances_after = replacement.balances;
        action.scale_before = pool.cached_price_scale; action.scale_after = replacement.cached_price_scale;
        action.debt_before = yb.state().debt; action.debt_after = replacement_yb.state().debt;
        action.collateral_before = yb.state().collateral; action.collateral_after = replacement_yb.state().collateral;
        action.cash_before = yb.state().stable_balance; action.cash_after = replacement_yb.state().stable_balance;
        action.vp_before = pool.virtual_price; action.vp_after = replacement.virtual_price;
        action.xcp_before = pool.xcp_profit; action.xcp_after = replacement.xcp_profit;
        if (!equal) {
            pool = std::move(replacement); yb = std::move(replacement_yb);
            action.segment = ++summary.resets;
        }
        log(std::move(action)); requested_ = false;
        return !equal;
    }
    ReconciliationSummary summary;
private:
    static constexpr uint64_t NS = 1'000'000'000ULL;
    const events::ObservedStateTape<T>& tape_;
    StateReconciliationMode mode_;
    T threshold_;
    uint64_t delay_ns_, detection_ns_{}, deadline_ns_{};
    bool requested_{false};
    size_t next_{};
    const events::ObservedState<T>* latest_{nullptr};
    StateReconciliationAction<T> record(const char* phase, uint64_t wall) const {
        StateReconciliationAction<T> a; a.phase = phase; a.wall_ns = wall;
        a.source_block = latest_->source_block; a.source_timestamp = latest_->source_timestamp;
        a.available_ns = latest_->available_ns;
        a.detection_ns = detection_ns_; a.deadline_ns = deadline_ns_;
        a.segment = summary.resets;
        return a;
    }
};
} // namespace arb::harness
