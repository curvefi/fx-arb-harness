// Pool runner - single pool execution and simulation runner (transport-free).
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "core/common.hpp"
#include "events/types.hpp"
#include "harness/metrics.hpp"
#include "harness/actions.hpp"
#include "harness/detailed_output.hpp"
#include "harness/donation.hpp"
#include "harness/idle_tick.hpp"
#include "harness/user_swap.hpp"
#include "harness/run_config.hpp"
#include "harness/event_loop.hpp"
#include "pools/pool_init.hpp"
#include "pools/twocrypto_fx/twocrypto.hpp"
#include "trading/costs.hpp"

namespace arb {
namespace harness {

template <typename T>
struct InitialMarketAnchor {
    T price{T(0)};
    uint64_t ts{0};
    bool from_event{false};
};

template <typename T>
InitialMarketAnchor<T> initial_market_anchor(
    const EventSoA& events,
    const T& fallback_price,
    uint64_t start_ts = 0
) {
    for (size_t i = 0; i < events.size(); ++i) {
        if (start_ts != 0 && events.ts[i] < start_ts) {
            continue;
        }
        if (events.p_cex[i] > 0.0) {
            return {static_cast<T>(events.p_cex[i]), events.ts[i], true};
        }
    }
    if (fallback_price > T(0)) {
        return {fallback_price, 0, false};
    }
    return {T(1), 0, false};
}

template <typename T>
bool looks_balanced_at_price(const T& liq0, const T& liq1, const T& price) {
    if (!(liq0 > T(0)) || !(liq1 > T(0)) || !(price > T(0))) {
        return false;
    }
    const T risky_value = liq1 * price;
    if (!(risky_value > T(0))) {
        return false;
    }
    const T diff = liq0 > risky_value ? liq0 - risky_value : risky_value - liq0;
    const T denom = liq0 > risky_value ? liq0 : risky_value;
    return diff / denom < T(1e-6);
}

// Result from running a single pool - includes all metrics
template <typename T>
struct PoolResult {
    std::string tag;
    size_t pool_index{0};

    // Core trading metrics
    Metrics<T> metrics{};

    // Time-weighted metrics
    TimeWeightedMetrics<T> tw_metrics{};

    // Slippage probes
    SlippageProbes<T> slippage_probes{};

    // Start/end timestamps
    uint64_t t_start{0};
    uint64_t t_end{0};

    // Initial state (for APY calculations)
    T tvl_start{0};
    T donation_apy{0};
    T donation_frequency{0};
    double apy_net_gm{-1.0};
    bool yb_releverage_enabled{false};
    T yb_releverage_fee{T(0)};
    double yb_releverage_apy{-1.0};
    double yb_releverage_apy_gm{-1.0};
    double yb_releverage_final_growth{-1.0};
    uint64_t yb_releverage_trades{0};
    uint64_t yb_releverage_gm_windows{0};
    uint64_t yb_releverage_gm_floored_windows{0};
    double yb_releverage_gm_floor_share{-1.0};
    double lp_gm_90d_worst_window{-1.0};
    double lp_gm_90d_median_window{-1.0};
    double lp_gm_90d_floor_share{-1.0};
    double yb_gm_90d_worst_window{-1.0};
    double yb_gm_90d_median_window{-1.0};
    double yb_gm_90d_floor_share{-1.0};
    double yb_gm_30d_cvar20{-1.0};
    double yb_gm_30d_floor_share{-1.0};
    double yb_growth_step_share_1d{-1.0};
    double yb_growth_step_share_3d{-1.0};
    double yb_growth_step_share_7d{-1.0};
    double yb_growth_top10_step_share{-1.0};
    double max_abs_drift_1d{-1.0};
    double max_abs_drift_3d{-1.0};
    double drift_3d_time_share_over_2pct{-1.0};
    double drift_stall_tau_max_days{-1.0};
    double drift_stalled_share{-1.0};

    // Final pool state
    std::array<T, 2> balances{T(0), T(0)};
    std::array<T, 2> admin_balances{T(0), T(0)};
    T D{0};
    T totalSupply{0};
    T price_scale{0};
    T price_oracle{0};
    T virtual_price{0};
    T xcp_profit{0};
    T lp_xcp_profit{0};
    T vp_boosted{0};
    T donation_shares{0};
    T donation_unlocked{0};
    T last_prices{0};
    uint64_t timestamp{0};
    pools::twocrypto_fx::PolicyHookMetrics<T> policy_hook_metrics{};

    // Timing
    double elapsed_ms{0};

    // Success flag
    bool success{false};
    std::string error_msg;

    double duration_s() const {
        return (t_end > t_start) ? static_cast<double>(t_end - t_start) : 0.0;
    }
};

// Run a single pool configuration and return results
template <typename T>
PoolResult<T> run_single_pool(
    const pools::PoolInit<T>& pool_init,
    const trading::Costs<T>& costs,
    const EventSoA& events,
    const RunConfig<T>& cfg,
    const std::vector<Candle>* candles = nullptr,
    std::vector<Action<T>>* out_actions = nullptr,
    std::vector<DetailedEntry<T>>* out_detailed_entries = nullptr
) {
    using Pool = pools::twocrypto_fx::TwoCryptoPool<T>;

    PoolResult<T> result;
    result.tag = pool_init.tag;
    result.pool_index = pool_init.global_index;

    auto t_start = std::chrono::high_resolution_clock::now();
    try {
        const uint64_t requested_start_ts = pool_init.start_ts;
        const bool restore_historical = pool_init.historical_state.enabled;
        const auto anchor = restore_historical
            ? InitialMarketAnchor<T>{
                pool_init.historical_state.price_scale,
                pool_init.historical_state.source_timestamp,
                false,
            }
            : initial_market_anchor<T>(
                events,
                pool_init.initial_price,
                requested_start_ts
            );
        const T initial_price = anchor.price;

        Pool pool(
            pool_init.precisions,
            pool_init.A,
            pool_init.gamma,
            pool_init.mid_fee,
            pool_init.out_fee,
            pool_init.fee_gamma,
            pool_init.adjustment_step_min,
            pool_init.adjustment_step_max,
            pool_init.ma_time,
            initial_price,
            pool_init.reserved_profit_fraction,
            pool_init.admin_fee,
            pool_init.policy_kind,
            pool_init.policy_config
        );
        pool.donation_duration = pool_init.donation_duration;

        uint64_t init_ts = requested_start_ts;
        if (init_ts == 0 && !events.empty()) {
            init_ts = events.ts.front();
        }
        if (init_ts == 0) {
            init_ts = 1700000000ULL;
        }
        if (
            restore_historical &&
            init_ts < pool_init.historical_state.source_timestamp
        ) {
            throw std::invalid_argument(
                "run start precedes historical pool checkpoint"
            );
        }
        pool.set_block_timestamp(init_ts);

        if (restore_historical) {
            const auto& state = pool_init.historical_state;
            pool.balances = state.balances;
            pool.admin_balances = state.admin_balances;
            pool.last_admin_fee_claim_timestamp =
                state.last_admin_fee_claim_timestamp;
            pool.D = state.D;
            pool.totalSupply = state.total_supply;
            pool.cached_price_scale = state.price_scale;
            pool.cached_price_oracle = state.price_oracle;
            pool.last_prices = state.last_prices;
            pool.last_timestamp = state.last_timestamp;
            pool.virtual_price = state.virtual_price;
            pool.xcp_profit = state.xcp_profit;
            pool.lp_xcp_profit = state.lp_xcp_profit;
            pool.donation_shares = state.donation_shares;
            pool.last_donation_release_ts = state.last_donation_release_ts;
            pool.donation_protection_expiry_ts = state.donation_protection_expiry_ts;
            pool.donation_protection_period = state.donation_protection_period;
            pool.donation_protection_lp_threshold = state.donation_protection_lp_threshold;
            pool.donation_protection_extension_remainder =
                state.donation_protection_extension_remainder;
            pool.donation_shares_max_ratio = state.donation_shares_max_ratio;
            pool.cached_ema_dt = 0;
            pool.cached_ema_alpha = T(0);
            pool.cached_ema_alpha_valid = false;
            pool.initialize_policy_state_from_pool();
        } else {
            T liq0 = pool_init.initial_liq[0];
            T liq1 = pool_init.initial_liq[1];
            if (liq0 <= T(0) || liq1 <= T(0)) {
                liq0 = T(1000000.0);
                liq1 = liq0 / initial_price;
            } else if (
                anchor.from_event &&
                pool_init.initial_price > T(0) &&
                pool_init.initial_price != initial_price &&
                looks_balanced_at_price(liq0, liq1, pool_init.initial_price)
            ) {
                liq1 = liq0 / initial_price;
            }
            pool.add_liquidity({liq0, liq1}, T(0));
        }

        if (
            pool_init.initial_donation_days > T(0) &&
            pool_init.donation_apy > T(0)
        ) {
            constexpr T SEC_PER_YEAR = static_cast<T>(365.0 * 86400.0);
            const T seed_s =
                pool_init.initial_donation_days * static_cast<T>(86400.0);
            const T frac = pool_init.donation_apy * seed_s / SEC_PER_YEAR;
            const T po = pool.cached_price_oracle;
            const T tvl = pool.balances[0] + pool.balances[1] * po;
            const T coin0_equiv_amt = tvl * frac;
            const T ratio1 = std::clamp<T>(pool_init.donation_coins_ratio, T(0), T(1));
            const T amt0 = (T(1) - ratio1) * coin0_equiv_amt;
            const T amt1 = (po > T(0)) ? (ratio1 * coin0_equiv_amt / po) : T(0);
            if (amt0 > T(0) || amt1 > T(0)) {
                (void)pool.add_liquidity({amt0, amt1}, T(0), true);
                const T vested_elapsed = pool.donation_duration;
                pool.last_donation_release_ts = T(init_ts) - vested_elapsed;
            }
        }

        DonationCfg<T> dcfg{};
        if (pool_init.donation_apy > T(0) && pool_init.donation_frequency > T(0) && !events.empty()) {
            uint64_t donation_start_ts = init_ts;
            dcfg.init(
                pool_init.donation_apy,
                pool_init.donation_frequency,
                pool_init.donation_coins_ratio,
                donation_start_ts
            );
        }

        IdleTickCfg<T> icfg{};
        icfg.freq_s = cfg.dustswap_freq_s;
        icfg.randomize = cfg.dustswap_random;
        if (icfg.randomize && icfg.freq_s < IdleTickCfg<T>::RANDOM_MIN_INTERVAL_S) {
            throw std::invalid_argument(
                "dustswap_random requires dustswap_freq_s >= 60"
            );
        }
        if (
            icfg.freq_s > 0 && cfg.dustswap_dynamic_freq_s > 0 &&
            !cfg.allow_hybrid_keeper
        ) {
            throw std::invalid_argument(
                "dustswap_freq_s and dustswap_dynamic_freq_s are mutually exclusive"
            );
        }
        const unsigned dynamic_modes =
            static_cast<unsigned>(cfg.dustswap_dynamic_freq_s > 0) +
            static_cast<unsigned>(
                cfg.dustswap_dynamic_gap_enabled ||
                cfg.dustswap_commit_clock_freq_s > 0
            ) +
            static_cast<unsigned>(cfg.policy_keeper_enabled);
        if (dynamic_modes > 1) {
            throw std::invalid_argument(
                "dynamic, gap/commit-clock, and policy keepers are mutually exclusive"
            );
        }
        if (
            icfg.freq_s > 0 &&
            (cfg.dustswap_dynamic_gap_enabled ||
             cfg.dustswap_commit_clock_freq_s > 0 ||
             cfg.policy_keeper_enabled)
        ) {
            throw std::invalid_argument(
                "scheduleless keepers require dustswap_freq_s=0"
            );
        }
        if (
            !cfg.dustswap_dynamic_gap_enabled &&
            cfg.dustswap_dynamic_heartbeat_s > 0
        ) {
            throw std::invalid_argument(
                "dynamic keeper heartbeat requires gap mode"
            );
        }

        UserSwapCfg<T> ucfg{};
        ucfg.freq_s = cfg.user_swap_freq_s;
        ucfg.size_frac = pool_init.user_swap_size_frac
            ? *pool_init.user_swap_size_frac
            : cfg.user_swap_size_frac;
        ucfg.thresh = cfg.user_swap_thresh;
        if (ucfg.enabled() && !events.empty()) {
            ucfg.init(init_ts);
        }

        auto loop_result = run_event_loop(
            pool, events, costs, dcfg, icfg, ucfg, cfg,
            candles,
            restore_historical ? init_ts : 0,
            out_actions,
            out_detailed_entries
        );

        result.metrics = loop_result.metrics;
        result.tw_metrics = loop_result.tw_metrics;
        result.slippage_probes = loop_result.slippage_probes;
        result.t_start = loop_result.t_start;
        result.t_end = loop_result.t_end;
        result.tvl_start = loop_result.tvl_start;
        result.donation_apy = loop_result.donation_apy;
        result.donation_frequency = pool_init.donation_frequency;
        result.apy_net_gm = loop_result.apy_net_gm;
        result.yb_releverage_enabled = loop_result.yb_releverage_enabled;
        result.yb_releverage_fee = loop_result.yb_releverage_fee;
        result.yb_releverage_apy = loop_result.yb_releverage_apy;
        result.yb_releverage_apy_gm = loop_result.yb_releverage_apy_gm;
        result.yb_releverage_final_growth = loop_result.yb_releverage_final_growth;
        result.yb_releverage_trades = loop_result.yb_releverage_trades;
        result.yb_releverage_gm_windows = loop_result.yb_releverage_gm_windows;
        result.yb_releverage_gm_floored_windows =
            loop_result.yb_releverage_gm_floored_windows;
        result.yb_releverage_gm_floor_share =
            loop_result.yb_releverage_gm_floor_share;
        result.lp_gm_90d_worst_window = loop_result.lp_gm_90d_worst_window;
        result.lp_gm_90d_median_window = loop_result.lp_gm_90d_median_window;
        result.lp_gm_90d_floor_share = loop_result.lp_gm_90d_floor_share;
        result.yb_gm_90d_worst_window = loop_result.yb_gm_90d_worst_window;
        result.yb_gm_90d_median_window = loop_result.yb_gm_90d_median_window;
        result.yb_gm_90d_floor_share = loop_result.yb_gm_90d_floor_share;
        result.yb_gm_30d_cvar20 = loop_result.yb_gm_30d_cvar20;
        result.yb_gm_30d_floor_share = loop_result.yb_gm_30d_floor_share;
        result.yb_growth_step_share_1d = loop_result.yb_growth_step_share_1d;
        result.yb_growth_step_share_3d = loop_result.yb_growth_step_share_3d;
        result.yb_growth_step_share_7d = loop_result.yb_growth_step_share_7d;
        result.yb_growth_top10_step_share = loop_result.yb_growth_top10_step_share;
        result.max_abs_drift_1d = loop_result.max_abs_drift_1d;
        result.max_abs_drift_3d = loop_result.max_abs_drift_3d;
        result.drift_3d_time_share_over_2pct =
            loop_result.drift_3d_time_share_over_2pct;
        result.drift_stall_tau_max_days = loop_result.drift_stall_tau_max_days;
        result.drift_stalled_share = loop_result.drift_stalled_share;

        result.balances[0] = pool.balances[0];
        result.balances[1] = pool.balances[1];
        result.admin_balances[0] = pool.admin_balances[0];
        result.admin_balances[1] = pool.admin_balances[1];
        result.D = pool.D;
        result.totalSupply = pool.totalSupply;
        result.price_scale = pool.cached_price_scale;
        result.price_oracle = pool.cached_price_oracle;
        result.virtual_price = pool.get_virtual_price();
        result.xcp_profit = pool.xcp_profit;
        result.lp_xcp_profit = pool.lp_xcp_profit;
        result.vp_boosted = pool.get_vp_boosted();
        result.donation_shares = pool.donation_shares;
        result.donation_unlocked = pool.donation_unlocked();
        result.last_prices = pool.last_prices;
        result.timestamp = pool.block_timestamp;
        result.policy_hook_metrics = pool.policy_hook_metrics;
        result.success = true;

    } catch (const std::exception& e) {
        result.success = false;
        result.error_msg = e.what();
    } catch (...) {
        result.success = false;
        result.error_msg = "Unknown error";
    }

    auto t_end = std::chrono::high_resolution_clock::now();
    result.elapsed_ms = std::chrono::duration<double, std::milli>(t_end - t_start).count();

    return result;
}

} // namespace harness
} // namespace arb
