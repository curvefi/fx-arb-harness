// Logging utilities for action recording and detailed output (arena-backed).
#pragma once

#include <array>
#include <vector>

#include "harness/actions.hpp"
#include "harness/detailed_output.hpp"
#include "pools/twocrypto_fx/helpers.hpp"

namespace arb {
namespace harness {

// Optional policy-owned trace diagnostics; never evaluated in summary grids.
template <typename Policy, typename Model>
auto policy_fee_diagnostics(const Model& model, uint64_t ts, int)
    -> decltype(Policy::fee_diagnostics(model.compiled_state, model.params, ts), std::array<double,3>{}) {
    const auto values=Policy::fee_diagnostics(model.compiled_state,model.params,ts);
    return std::array<double,3>{static_cast<double>(values[0]),static_cast<double>(values[1]),static_cast<double>(values[2])};
}
template <typename Policy, typename Model>
std::array<double,3> policy_fee_diagnostics(const Model&, uint64_t, long) { return {-1,-1,-1}; }

template <typename Policy, typename Model>
auto policy_price_diagnostics(const Model& model, int)
    -> decltype(Policy::price_diagnostics(model.compiled_state), std::array<double,3>{}) {
    const auto v = Policy::price_diagnostics(model.compiled_state);
    return {double(v[0]), double(v[1]), double(v[2])};
}
template <typename Policy, typename Model>
std::array<double,3> policy_price_diagnostics(const Model&, long) { return {-1,-1,-1}; }

// Optional pressure-policy observations. No writes or work in summary grids.
template <typename Policy, typename Model>
auto policy_pressure_diagnostics(const Model& model, uint64_t ts, int)
    -> decltype(Policy::projected(model.compiled_state, model.params, ts),
                model.compiled_state.pressure.fresh_bumps,
                model.compiled_state.pressure.cached_bumps, std::array<double,4>{}) {
    const auto pressure = Policy::projected(model.compiled_state, model.params, ts);
    return {double(pressure[0]), double(pressure[1]),
            double(model.compiled_state.pressure.fresh_bumps),
            double(model.compiled_state.pressure.cached_bumps)};
}
template <typename Policy, typename Model>
std::array<double,4> policy_pressure_diagnostics(const Model&, uint64_t, long) {
    return {-1,-1,-1,-1};
}

// ActionLogger: writes directly into an external trace buffer when enabled.
// When out_actions is nullptr, enabled() is false and all methods are zero-cost no-ops.
template <typename T>
class ActionLogger {
public:
    ActionLogger() : out_actions_(nullptr) {}
    explicit ActionLogger(std::vector<Action<T>>* out_actions)
        : out_actions_(out_actions) {}

    bool enabled() const { return out_actions_ != nullptr; }

    // Log a donation action
    template <typename DonRes, typename DCfg>
    void log_donation(uint64_t ts, const DonRes& don_res, const DCfg& dcfg) {
        if (!enabled()) return;
        DonationAction<T> act;
        act.ts = ts;
        act.ts_due = don_res.ts_due;
        act.amounts = don_res.amounts;
        act.price_scale = don_res.price_scale;
        act.donation_ratio1 = dcfg.ratio1;
        act.apy_per_year = dcfg.apy;
        act.freq_s = dcfg.freq_s;
        out_actions_->push_back(std::move(act));
    }

    void log_yb_donation(
        uint64_t ts,
        T amount_coin0,
        T price_scale,
        T donation_apy
    ) {
        if (!enabled()) return;
        DonationAction<T> act;
        act.ts = ts;
        act.ts_due = ts;
        act.amounts = {amount_coin0, T(0)};
        act.price_scale = price_scale;
        act.donation_ratio1 = T(0);
        act.apy_per_year = donation_apy;
        act.freq_s = 0;
        out_actions_->push_back(std::move(act));
    }

    // Log a tick action (idle tick with no trade)
    template <typename Pool>
    void log_tick(uint64_t ts, T p_cex,
                  T ps_before, T oracle_before, T xcp_profit_before, T vp_before,
                  const Pool& pool) {
        if (!enabled()) return;
        TickAction<T> act;
        act.ts = ts;
        act.p_cex = p_cex;
        act.ps_before = ps_before;
        act.ps_after = pool.cached_price_scale;
        act.oracle_before = oracle_before;
        act.oracle_after = pool.cached_price_oracle;
        act.xcp_profit_before = xcp_profit_before;
        act.xcp_profit_after = pool.xcp_profit;
        act.vp_before = vp_before;
        act.vp_after = pool.get_vp_boosted();
        out_actions_->push_back(std::move(act));
    }

    // Log an arb exchange action
    template <typename Pool>
    void log_exchange(uint64_t ts, int i, int j, T dx, T dy_after_fee, T fee_tokens,
                      T profit_coin0, T p_cex, T p_pool_before,
                      T oracle_before, T ps_before, uint64_t last_ts_before, T lp_before,
                      T xcp_profit_before, T vp_before,
                      const Pool& pool, bool synthetic_user = false) {
        if (!enabled()) return;
        ExchangeAction<T> act;
        act.synthetic_user = synthetic_user;
        act.ts = ts;
        act.i = i;
        act.j = j;
        act.dx = dx;
        act.dy_after_fee = dy_after_fee;
        act.fee_tokens = fee_tokens;
        act.profit_coin0 = profit_coin0;
        act.p_cex = p_cex;
        act.p_pool_before = p_pool_before;
        act.p_pool_after = pool.get_p();
        act.oracle_before = oracle_before;
        act.oracle_after = pool.cached_price_oracle;
        act.ps_before = ps_before;
        act.ps_after = pool.cached_price_scale;
        act.last_ts_before = last_ts_before;
        act.last_ts_after = pool.last_timestamp;
        act.lp_before = lp_before;
        act.lp_after = pool.last_prices;
        act.xcp_profit_before = xcp_profit_before;
        act.xcp_profit_after = pool.xcp_profit;
        act.vp_before = vp_before;
        act.vp_after = pool.get_vp_boosted();
        act.balance_indicator = pools::twocrypto_fx::balance_indicator(pool);
        act.balances_after = pool.balances;
        act.D_after = pool.D;
        act.lp_xcp_profit_after = pool.lp_xcp_profit;
        act.donation_shares_after = pool.donation_shares;
        act.total_supply_after = pool.totalSupply;
        act.virtual_price_after = pool.get_virtual_price();
        act.last_donation_release_ts_after = pool.last_donation_release_ts;
        act.donation_protection_expiry_ts_after = pool.donation_protection_expiry_ts;
        out_actions_->push_back(std::move(act));
    }

    // Log an active_2l fill (its input and output) and the public state it left
    template <typename Pool>
    void log_injected(uint64_t ts, size_t index, const std::array<T, 2>& out, const Pool& pool) {
        if (!enabled()) return;
        InjectedLog<T> act;
        act.ts = ts;
        act.index = index;
        act.out = out;
        act.balances = pool.balances;
        act.D = pool.D;
        act.total_supply = pool.totalSupply;
        act.price_scale = pool.cached_price_scale;
        act.price_oracle = pool.cached_price_oracle;
        act.last_prices = pool.last_prices;
        act.last_timestamp = pool.last_timestamp;
        act.virtual_price = pool.get_virtual_price();
        act.xcp_profit = pool.xcp_profit;
        act.lp_xcp_profit = pool.lp_xcp_profit;
        act.donation_shares = pool.donation_shares;
        act.last_donation_release_ts = pool.last_donation_release_ts;
        act.donation_protection_expiry_ts = pool.donation_protection_expiry_ts;
        out_actions_->push_back(std::move(act));
    }

    // The LEVAMM state after the last logged fill.
    void annotate_last_yb(uint8_t direction, T collateral, T debt, T stable_balance, T price, T donation) {
        if (!enabled()) return;
        auto& act = std::get<InjectedLog<T>>(out_actions_->back());
        act.yb_direction = direction;
        act.yb_collateral = collateral;
        act.yb_debt = debt;
        act.yb_stable_balance = stable_balance;
        act.yb_price = price;
        act.yb_donation = donation;
    }

private:
    std::vector<Action<T>>* out_actions_{nullptr};
};

// DetailedLogger: writes directly into an external trace buffer when enabled.
// When out_entries is nullptr, enabled() is false and logging is a zero-cost no-op.
template <typename T>
class DetailedLogger {
public:
    DetailedLogger() : out_entries_(nullptr), interval_(1) {}
    explicit DetailedLogger(std::vector<DetailedEntry<T>>* out_entries, size_t interval = 1)
        : out_entries_(out_entries), interval_(interval > 0 ? interval : 1) {}

    bool enabled() const { return out_entries_ != nullptr; }

    // Log current pool state for this event (respects interval)
    template <typename Pool>
    bool log_event(const Pool& pool, uint64_t ts, const Candle& candle, T p_cex,
                   T p_price_feed,
                   T donation_apy, uint64_t n_trades, uint64_t n_rebalances,
                   T slippage_1pct_0to1, T slippage_1pct_1to0) {
        if (!enabled()) return false;

        if (event_count_++ % interval_ != 0) return false;

        DetailedEntry<T> entry;
        entry.t = ts;
        entry.token0 = pool.balances[0];
        entry.token1 = pool.balances[1];
        entry.D = pool.D;
        const auto xp = pools::twocrypto_fx::pool_xp_current(pool);
        entry.xp_0 = xp[0];
        entry.xp_1 = xp[1];
        entry.price_oracle = pool.cached_price_oracle;
        entry.price_scale = pool.cached_price_scale;
        entry.vp = pool.get_virtual_price();
        entry.vp_boosted = pool.get_vp_boosted();
        entry.profit = entry.vp - T(1);
        entry.xcp = pool.xcp_profit;
        entry.lp_xcp_profit = pool.lp_xcp_profit;
        entry.total_supply = pool.totalSupply;
        entry.donation_apy = donation_apy;
        entry.donation_shares = pool.donation_shares;
        entry.donation_unlocked = pool.donation_unlocked();
        entry.last_prices = pool.last_prices;
        entry.last_timestamp = pool.last_timestamp;
        entry.open = static_cast<T>(candle.open);
        entry.high = static_cast<T>(candle.high);
        entry.low = static_cast<T>(candle.low);
        entry.close = static_cast<T>(candle.close);
        entry.p_cex = p_cex;
        entry.p_price_feed = p_price_feed;
        entry.fee = pools::twocrypto_fx::viewer_exchange_fee_fraction(pool, p_cex);
#ifdef TWOCRYPTO_POLICY_HEADER
        const auto fees=policy_fee_diagnostics<pools::twocrypto_fx::ChallengeFeePolicy<T>>(pool.policy,ts,0);
        entry.policy_base_fee=fees[0];
        entry.policy_fallback_fee=fees[1];
        entry.policy_fee_signal=fees[2];
        const auto pressure = policy_pressure_diagnostics<pools::twocrypto_fx::ChallengeFeePolicy<T>>(pool.policy,ts,0);
        entry.policy_pressure_base=pressure[0];
        entry.policy_pressure_fallback=pressure[1];
        entry.policy_pressure_fresh_bumps=pressure[2];
        entry.policy_pressure_cached_bumps=pressure[3];
        const auto price = policy_price_diagnostics<pools::twocrypto_fx::ChallengeFeePolicy<T>>(pool.policy,0);
        entry.policy_target_calls=price[0];
        entry.policy_actuator_holds=price[1];
        entry.policy_gate_rejections=price[2];
#endif
        entry.slippage_1pct_0to1 = slippage_1pct_0to1;
        entry.slippage_1pct_1to0 = slippage_1pct_1to0;
        entry.n_trades = n_trades;
        entry.n_rebalances = n_rebalances;
        entry.yb_initialized = 0;
        entry.yb_growth = MetricF<T>(0);
        entry.yb_fee = MetricF<T>(0);
        entry.yb_releverage_trades = 0;
        entry.yb_stable_balance = T(0);
        entry.yb_debt = T(0);
        entry.yb_collateral_lp = T(0);
        entry.yb_lp_oracle = T(0);
        entry.yb_lp_fair = T(0);
        out_entries_->push_back(entry);
        return true;
    }

    void annotate_last_yb(
        bool initialized,
        MetricF<T> growth,
        MetricF<T> fee,
        uint64_t trades
    ) {
        if (!enabled() || out_entries_->empty()) return;
        auto& entry = out_entries_->back();
        entry.yb_initialized = initialized ? uint64_t(1) : uint64_t(0);
        entry.yb_growth = growth;
        entry.yb_fee = fee;
        entry.yb_releverage_trades = trades;
    }

    void annotate_last_yb_position(
        const T& stable_balance,
        const T& debt,
        const T& collateral_lp,
        const T& lp_oracle,
        const T& lp_fair
    ) {
        if (!enabled() || out_entries_->empty()) return;
        auto& entry = out_entries_->back();
        entry.yb_stable_balance = stable_balance;
        entry.yb_debt = debt;
        entry.yb_collateral_lp = collateral_lp;
        entry.yb_lp_oracle = lp_oracle;
        entry.yb_lp_fair = lp_fair;
    }

private:
    std::vector<DetailedEntry<T>>* out_entries_{nullptr};
    size_t interval_{1};
    size_t event_count_{0};
};

} // namespace harness
} // namespace arb
