// Logging utilities for action recording and detailed output (arena-backed).
#pragma once

#include <vector>

#include "harness/actions.hpp"
#include "harness/detailed_output.hpp"
#include "pools/twocrypto_fx/helpers.hpp"

namespace arb {
namespace harness {

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
        out_actions_->push_back(std::move(act));
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
