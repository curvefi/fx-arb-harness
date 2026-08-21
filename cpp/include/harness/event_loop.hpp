// Event loop for processing price events and executing arb trades
#pragma once

#include <algorithm>
#include <cmath>
#include <optional>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "core/common.hpp"
#include "events/types.hpp"
#include "harness/metrics.hpp"
#include "harness/actions.hpp"
#include "harness/detailed_output.hpp"
#include "harness/logging.hpp"
#include "harness/donation.hpp"
#include "harness/idle_tick.hpp"
#include "harness/yb_2l.hpp"
#include "harness/yb_2l_apy.hpp"
#include "harness/pool_snapshot.hpp"
#include "harness/run_config.hpp"
#include "harness/user_swap.hpp"
#include "trading/costs.hpp"
#include "trading/arbitrageur.hpp"
#include "pools/twocrypto_fx/helpers.hpp"

namespace arb {
namespace harness {

template <typename T, typename Pool>
EventLoopResult<T> run_event_loop(
    Pool& pool,
    const EventSoA& events,
    const trading::Costs<T>& costs,
    DonationCfg<T>& dcfg,
    IdleTickCfg<T>& icfg,
    UserSwapCfg<T>& ucfg,
    const RunConfig<T>& cfg,
    const std::vector<Candle>* candles = nullptr,
    uint64_t event_start_floor_ts = 0,
    std::vector<Action<T>>* out_actions = nullptr,
    std::vector<DetailedEntry<T>>* out_detailed_entries = nullptr
) {
    const T min_swap_frac = cfg.min_swap_frac;
    const T max_swap_frac = cfg.max_swap_frac;
    const bool enable_slippage_probes = cfg.enable_slippage_probes;
    const size_t detailed_interval = cfg.detailed_interval;
    const T yb_releverage_fee = cfg.yb_releverage_fee;
    const YbMode yb_mode = cfg.yb_mode;

    // Passive mode re-runs the exact active_2l transition on a private copy
    // of the initial pool/configs after the primary loop completes. The
    // primary pool and schedulers are mutated by the loop, so their pre-loop
    // state is captured once here; the shadow result contributes only the yb
    // metric family.
    std::optional<Pool> passive_pool_initial;
    DonationCfg<T> passive_dcfg_initial{};
    UserSwapCfg<T> passive_ucfg_initial{};
    if (yb_mode == YbMode::Passive) {
        passive_pool_initial.emplace(pool);
        passive_dcfg_initial = dcfg;
        passive_ucfg_initial = ucfg;
    }

    using F = MetricF<T>;
    EventLoopResult<T> result{};
    Metrics<T>& m = result.metrics;
    TimeWeightedMetrics<T>& tw = result.tw_metrics;
    SlippageProbes<T>& sp = result.slippage_probes;

    const size_t n_events = events.size();
    if (n_events == 0) return result;
    const size_t first_event_idx = event_start_floor_ts == 0
        ? 0
        : static_cast<size_t>(std::lower_bound(
            events.ts.begin(), events.ts.end(), event_start_floor_ts
        ) - events.ts.begin());
    if (first_event_idx == n_events) return result;

    result.t_start = events.ts[first_event_idx];
    result.t_end = events.ts[n_events - 1];

    result.tvl_start = pool.balances[0] + pool.balances[1] * pool.cached_price_scale;
    result.donation_apy = dcfg.apy;

    RollingGeoApy90d<F> apy_net_gm;
    RollingGeoApy90d<F> yb_apy_gm;
    RollingGeoApyWindow<F> apy_net_gm_90d(90ULL * 24ULL * 60ULL * 60ULL, F(0.02));
    RollingGeoApyWindow<F> yb_apy_gm_90d(90ULL * 24ULL * 60ULL * 60ULL, F(0.01));
    RollingGeoApyWindow<F> yb_apy_gm_30d(30ULL * 24ULL * 60ULL * 60ULL, F(0.01));
    MultiScalePositiveGrowthConcentration<F> yb_growth_steps;
    SignedDriftEma<F> drift_ema_1d(F(86400), F(0));
    SignedDriftEma<F> drift_ema_3d(F(3) * F(86400), F(0.02));
    DriftStallTracker<F> drift_stall;
    auto donation_growth_since_start = [&](uint64_t ts) -> F {
        const F elapsed_s = ts > result.t_start
            ? static_cast<F>(ts - result.t_start)
            : F(0);
        return donation_growth<F>(static_cast<F>(dcfg.apy), static_cast<F>(dcfg.freq_s), elapsed_s);
    };
    auto sample_apy_net_gm = [&](uint64_t ts) {
        if (!apy_net_gm.should_sample(ts)) {
            return;
        }
        const F donation_growth = donation_growth_since_start(ts);
        if (!(donation_growth > F(0))) {
            return;
        }
        const T lp_profit_growth = pool.lp_xcp_profit;
        const F net_lp_profit_growth =
            static_cast<F>(lp_profit_growth) / donation_growth;
        apy_net_gm.sample(ts, net_lp_profit_growth);
        apy_net_gm_90d.sample(ts, net_lp_profit_growth);
    };
    sample_apy_net_gm(result.t_start);

    std::array<T, SlippageProbes<T>::N_SIZES> probe_sizes_coin0{};
    if (enable_slippage_probes) {
        for (size_t k = 0; k < SlippageProbes<T>::N_SIZES; ++k) {
            probe_sizes_coin0[k] = result.tvl_start * static_cast<T>(SlippageProbes<T>::SIZE_FRACS[k]);
        }
    }

    ActionLogger<T> action_logger(out_actions);
    DetailedLogger<T> detailed_logger(out_detailed_entries, detailed_interval);
    Yb2LActor<T> yb_2l_actor;
    if constexpr (std::is_floating_point_v<T>) {
        if (yb_mode == YbMode::Active2l) {
            yb_2l_actor = Yb2LActor<T>::fresh_2l(
                pool, dcfg.apy, yb_releverage_fee, result.t_start,
                cfg.yb_cash_multiplier
            );
        }
    } else if (yb_mode != YbMode::Off) {
        throw std::invalid_argument(
            "YieldBasis is available only on floating-point runtimes"
        );
    }
    Yb2LApyTracker<T> yb_2l_apy_tracker;

    if (detailed_logger.enabled() && candles == nullptr) {
        throw std::invalid_argument("candle samples requested but candles were not provided");
    }

    const T fee_cex = costs.arb_fee_bps / T(10000);
    const T cex_fee_discount = T(1) - fee_cex;
    const T cex_fee_markup = T(1) + fee_cex;

    auto sample_slippage_probes = [&](uint64_t ts, T p_cex) {
        if (!enable_slippage_probes || !(p_cex > T(0))) return;
        for (size_t k = 0; k < SlippageProbes<T>::N_SIZES; ++k) {
            sp.accumulate_previous(k, ts);

            const T S = probe_sizes_coin0[k];
            {
                auto pr = pools::twocrypto_fx::simulate_exchange_once(pool, 0, 1, S);
                const T dy1 = pr.first;
                const T ideal1 = S / p_cex;
                T s01 = T(0);
                if (ideal1 > T(0)) {
                    s01 = T(1) - (dy1 / ideal1);
                }
                const T dx1 = S / p_cex;
                auto pr10 = pools::twocrypto_fx::simulate_exchange_once(pool, 1, 0, dx1);
                const T dy0 = pr10.first;
                T s10 = T(0);
                if (S > T(0)) {
                    s10 = T(1) - (dy0 / S);
                }
                sp.sample(k, ts, s01, s10);
            }
        }
    };

    const bool edge_cacheable =
        pool.policy.kind == pools::twocrypto_fx::PolicyKind::None
        || pool.policy.quote_cache_safe();
    bool edge_valid = false;
    T edge_p_now{};
    T edge_fee{};
    std::array<T, 2> edge_xp{};
    const T omf_floor = std::max(T(1) - pool.fee_lower_bound(), T(1e-12));
    auto refresh_edge_inputs = [&]() {
        if (edge_cacheable && edge_valid) return;
        const auto xp_now = pools::twocrypto_fx::pool_xp_current(pool);
        edge_p_now = pools::twocrypto_fx::MathOps<T>::get_p(
            xp_now, pool.D, {pool.A, pool.gamma}
        ) * pool.cached_price_scale;
        edge_fee = pool.fee(xp_now);
        edge_xp = xp_now;
        edge_valid = true;
    };
    auto invalidate_edge_inputs = [&]() { edge_valid = false; };
    uint64_t last_tw_sample_ts = 0;
    bool have_tw_sample = false;
    auto sample_pre_trade = [&](uint64_t ts, T cex_price) {
        if (have_tw_sample && ts < last_tw_sample_ts + TimeWeightedMetrics<T>::PRICE_DIFF_BUCKET_S) {
            return;
        }
        last_tw_sample_ts = ts;
        have_tw_sample = true;

        tw.sample_price_error(ts, pool.cached_price_scale, cex_price);

        const T x0p = pool.balances[0];
        const T x1p = pool.balances[1] * cex_price;
        tw.sample_imbalance(ts, x0p, x1p);

        refresh_edge_inputs();
        tw.sample_fee(ts, edge_fee);
    };

    auto apply_donation = [&](std::size_t, uint64_t ts) {
        PoolTransactionSnapshot<Pool> transaction_snapshot(pool);
        auto don_res = make_donation_ex(pool, dcfg, ts, m);
        if (don_res.success) {
            invalidate_edge_inputs();
            action_logger.log_donation(ts, don_res, dcfg);
        } else {
            transaction_snapshot.restore(pool);
        }
    };

    auto apply_user_swap = [&](std::size_t, uint64_t ts, T cex_price) {
        if (try_user_swap(pool, ucfg, ts, cex_price)) {
            invalidate_edge_inputs();
        }
    };

    auto execute_arb = [&](size_t ev_idx, uint64_t ev_ts, T cex_price) -> bool {
        refresh_edge_inputs();
        if (!(omf_floor * (cex_fee_discount * cex_price) > edge_p_now) &&
            !(omf_floor * edge_p_now > cex_fee_markup * cex_price)) {
            return false;
        }
        T volume_cap = std::numeric_limits<T>::infinity();
        if (costs.use_volume_cap) {
            volume_cap = static_cast<T>(events.volume[ev_idx]) * costs.volume_cap_mult;
            if (!costs.volume_cap_is_coin1) {
                volume_cap *= cex_price;
            }
        }

        auto dec = trading::decide_trade(
            pool, cex_price, costs,
            volume_cap,
            min_swap_frac, max_swap_frac,
            cex_fee_discount, cex_fee_markup,
            &edge_p_now, &edge_fee, &edge_xp
        );
        if (dec.edge_seen) {
            m.arb_edge_candidates += 1;
        }
        if (dec.rejected_invalid_size) {
            m.arb_invalid_size_rejections += 1;
        }
        if (dec.rejected_nonpositive_profit) {
            m.arb_nonpositive_profit_rejections += 1;
            if (dec.profit < T(0)) {
                m.arb_guarded_loss_coin0 += -dec.profit;
            }
        }
        if (!dec.do_trade) {
            return false;
        }

        PoolTransactionSnapshot<Pool> transaction_snapshot(pool);
        try {
            const T ps_before = pool.cached_price_scale;
            T oracle_before{};
            T xcp_profit_before{};
            T vp_before{};
            T p_pool_before{};
            uint64_t last_ts_before{0};
            T lp_before{};
            const bool log_actions = action_logger.enabled();
            if (log_actions) {
                oracle_before = pool.cached_price_oracle;
                xcp_profit_before = pool.xcp_profit;
                vp_before = pool.get_vp_boosted();
                p_pool_before = pool.get_p();
                last_ts_before = pool.last_timestamp;
                lp_before = pool.last_prices;
            }

            invalidate_edge_inputs();
            auto res = pool.exchange_from_preview(
                static_cast<size_t>(dec.i),
                static_cast<size_t>(dec.j),
                dec.dx,
                dec.dy_after_fee,
                dec.fee_tokens
            );

            const T dy_after_fee = res[0];
            const T fee_tokens = res[1];
            const T ps_after = pool.cached_price_scale;

            m.trades += 1;
            m.notional += dec.notional_coin0;
            m.lp_fee_coin0 += (dec.j == 1 ? fee_tokens * cex_price : fee_tokens);
            m.arb_pnl_coin0 += dec.profit;

            const T gross_dy_tokens = dy_after_fee + fee_tokens;
            if (gross_dy_tokens > T(0) && dec.notional_coin0 > T(0)) {
                const T fee_frac = fee_tokens / gross_dy_tokens;
                m.fee_wsum += fee_frac * dec.notional_coin0;
                m.fee_w += dec.notional_coin0;
            }

            if (differs_rel(ps_after, ps_before)) {
                m.n_rebalances += 1;
            }

            if (enable_slippage_probes) {
                sample_slippage_probes(ev_ts, cex_price);
            }

            if (log_actions) {
                action_logger.log_exchange(ev_ts, dec.i, dec.j, dec.dx, dy_after_fee, fee_tokens,
                                           dec.profit, cex_price, p_pool_before,
                                           oracle_before, ps_before, last_ts_before, lp_before,
                                           xcp_profit_before, vp_before, pool);
            }
            return true;
        } catch (...) {
            transaction_snapshot.restore(pool);
            return false;
        }
    };

    auto apply_idle_tick = [&](std::size_t, uint64_t ts, T cex_price) -> bool {
        PoolTransactionSnapshot<Pool> transaction_snapshot(pool);
        const T ps_before = pool.cached_price_scale;
        T oracle_before{};
        T xcp_profit_before{};
        T vp_before{};
        const bool log_actions = action_logger.enabled();
        if (log_actions) {
            oracle_before = pool.cached_price_oracle;
            xcp_profit_before = pool.xcp_profit;
            vp_before = pool.get_vp_boosted();
        }

        const bool did_tick = try_idle_tick(pool, icfg, ts, m);
        if (!did_tick) {
            transaction_snapshot.restore(pool);
            return false;
        }
        ++m.fixed_keeper_ticks;
        if (pool.cached_price_scale != ps_before) {
            ++m.keeper_successful_submissions;
        }
        invalidate_edge_inputs();

        if (enable_slippage_probes) {
            sample_slippage_probes(ts, cex_price);
        }
        if (log_actions) {
            action_logger.log_tick(ts, cex_price, ps_before, oracle_before,
                                   xcp_profit_before, vp_before, pool);
        }
        return true;
    };

    auto apply_dynamic_idle_tick =
        [&](std::size_t, uint64_t ts, T cex_price) -> bool {
        const T ps_before = pool.cached_price_scale;
        const T oracle_before = pool.cached_price_oracle;
        const T xcp_profit_before = pool.xcp_profit;
        const T vp_before = action_logger.enabled() ? pool.get_vp_boosted() : T(0);

        ++m.dynamic_keeper_attempts;
        const bool scalar_snap_ok =
            pool.policy.kind == pools::twocrypto_fx::PolicyKind::None;
        bool committed = false;
        if (scalar_snap_ok) {
            PoolTransactionSnapshot<Pool> transaction_snapshot(pool);
            try {
                pool.tick();
                committed = pool.cached_price_scale != ps_before;
            } catch (...) {
                committed = false;
            }
            if (!committed) {
                transaction_snapshot.restore(pool);
            }
        } else {
            committed = try_commit_gated_tick(pool);
        }
        if (!committed) {
            return false;
        }

        const T ps_after = pool.cached_price_scale;
        if (differs_rel(ps_after, ps_before)) {
            ++m.n_rebalances;
        }
        ++m.dynamic_keeper_commits;
        ++m.keeper_successful_submissions;
        const double ps_before_d = static_cast<double>(ps_before);
        const double step_bps = ps_before_d > 0.0
            ? std::abs(static_cast<double>(ps_after) / ps_before_d - 1.0) * 10000.0
            : 0.0;
        m.dynamic_keeper_step_bps_sum += step_bps;
        m.dynamic_keeper_step_bps_max = std::max(
            m.dynamic_keeper_step_bps_max,
            step_bps
        );
        const std::array<double, 7> upper_bounds{1, 5, 10, 25, 50, 100, 200};
        std::size_t bin = upper_bounds.size();
        for (std::size_t i = 0; i < upper_bounds.size(); ++i) {
            if (step_bps <= upper_bounds[i]) {
                bin = i;
                break;
            }
        }
        ++m.dynamic_keeper_step_bps_histogram[bin];
        invalidate_edge_inputs();

        if (enable_slippage_probes) {
            sample_slippage_probes(ts, cex_price);
        }
        if (action_logger.enabled()) {
            action_logger.log_tick(ts, cex_price, ps_before, oracle_before,
                                   xcp_profit_before, vp_before, pool);
        }
        return true;
    };

    auto apply_gap_keeper_tick =
        [&](std::size_t, uint64_t ts, T cex_price,
            bool heartbeat_due, bool policy_owned) -> bool {
        const T ps_before = pool.cached_price_scale;
        const T oracle_before = pool.cached_price_oracle;
        const T xcp_profit_before = pool.xcp_profit;
        const T vp_before = action_logger.enabled() ? pool.get_vp_boosted() : T(0);

        ++m.dynamic_keeper_gap_checks;
        bool policy_submission = false;
        bool policy_attempt_threw = false;
#if !defined(ARB_POLICY_KEEPER_NO_PREFILTER_REFERENCE)
        if (policy_owned) {
            const auto preflight = pool.policy_keeper_preflight();
            ++m.policy_keeper_checks;
            if (!preflight.clock_ready) {
                ++m.policy_keeper_reject_clock;
            } else if (!preflight.trigger_evaluated ||
                       !preflight.decision.available ||
                       !(preflight.decision.raw_target > T(0))) {
                ++m.policy_keeper_reject_target_unavailable;
            } else {
                const auto& decision = preflight.decision;
                if (
                    decision.trigger_value_is_bps &&
                    decision.raw_gap_bps >= decision.threshold_bps
                ) {
                    ++m.policy_keeper_raw_gap_candidates;
                }
                if (!decision.ready) {
                    if (decision.shaped_target == ps_before) {
                        ++m.policy_keeper_reject_deadband;
                    } else if (decision.predicted_price_scale == ps_before) {
                        ++m.policy_keeper_reject_step_min;
                    } else {
                        ++m.policy_keeper_reject_below_threshold;
                    }
                } else if (!preflight.block_ready) {
                    ++m.policy_keeper_reject_block;
                } else if (!preflight.outer_profit_ready) {
                    ++m.policy_keeper_reject_outer_profit;
                } else {
                    policy_submission = true;
                    ++m.policy_keeper_submissions;
                }
            }
            if (!preflight.ready) {
                return false;
            }
        }
#endif
        typename Pool::KeeperGapProbe probe{};
        PoolTransactionSnapshot<Pool> transaction_snapshot(pool);
        try {
            probe = policy_owned
                ? pool.tick_policy_keeper()
                : pool.tick_keeper_gap(
                    cfg.dustswap_dynamic_gap_bps,
                    heartbeat_due
                );
        } catch (...) {
            transaction_snapshot.restore(pool);
            policy_attempt_threw = policy_owned;
        }
        if (policy_attempt_threw) {
            ++m.policy_keeper_exceptions;
            return false;
        }
        if (!probe.fired) {
            transaction_snapshot.restore(pool);
            if (policy_submission) {
                ++m.policy_keeper_unexpected_step_rejects;
            }
            return false;
        }

        ++m.dynamic_keeper_gap_fires;
        ++m.dynamic_keeper_attempts;
        if (probe.gap_fired) {
            ++m.dynamic_keeper_gap_threshold_fires;
        }
        if (probe.heartbeat_fired) {
            ++m.dynamic_keeper_heartbeat_fires;
        }

        if (pool.cached_price_scale == ps_before) {
            transaction_snapshot.restore(pool);
            if (policy_submission) {
                if (probe.prospective_lp_evaluated && !probe.lp_gate_passed) {
                    ++m.policy_keeper_final_lp_rejects;
                    if (probe.lp_below_precision) {
                        ++m.policy_keeper_lp_below_precision;
                    }
                    if (probe.lp_below_floor) {
                        ++m.policy_keeper_lp_below_floor;
                    }
                    if (probe.donation_burn_cap_exhausted) {
                        ++m.policy_keeper_lp_burn_cap_exhausted;
                    }
                } else {
                    ++m.policy_keeper_unexpected_step_rejects;
                }
            }
            return false;
        }

        const T ps_after = pool.cached_price_scale;
        if (differs_rel(ps_after, ps_before)) {
            ++m.n_rebalances;
        }
        ++m.dynamic_keeper_commits;
        ++m.keeper_successful_submissions;
        if (policy_submission) {
            ++m.policy_keeper_submitted_commits;
            if (ps_after > ps_before) {
                ++m.policy_keeper_direction_up;
            } else if (ps_after < ps_before) {
                ++m.policy_keeper_direction_down;
            }
        }
        const double ps_before_d = static_cast<double>(ps_before);
        const double step_bps = ps_before_d > 0.0
            ? std::abs(static_cast<double>(ps_after) / ps_before_d - 1.0) * 10000.0
            : 0.0;
        m.dynamic_keeper_step_bps_sum += step_bps;
        m.dynamic_keeper_step_bps_max = std::max(
            m.dynamic_keeper_step_bps_max,
            step_bps
        );
        const std::array<double, 7> upper_bounds{1, 5, 10, 25, 50, 100, 200};
        std::size_t bin = upper_bounds.size();
        for (std::size_t i = 0; i < upper_bounds.size(); ++i) {
            if (step_bps <= upper_bounds[i]) {
                bin = i;
                break;
            }
        }
        ++m.dynamic_keeper_step_bps_histogram[bin];
        invalidate_edge_inputs();

        if (enable_slippage_probes) {
            sample_slippage_probes(ts, cex_price);
        }
        if (action_logger.enabled()) {
            action_logger.log_tick(ts, cex_price, ps_before, oracle_before,
                                   xcp_profit_before, vp_before, pool);
        }
        return true;
    };

    const bool detailed_on = detailed_logger.enabled();
    const bool user_swap_on = ucfg.enabled();
    const bool yb_2l_on = yb_2l_actor.enabled();
    const bool donation_on = dcfg.enabled && !yb_2l_on;
    const bool dynamic_keeper_on = cfg.dustswap_dynamic_freq_s > 0;
    const bool gap_keeper_on = cfg.dustswap_dynamic_gap_enabled;
    const bool policy_keeper_on = cfg.policy_keeper_enabled;
    const bool gap_rate_limit_on =
        gap_keeper_on && cfg.dustswap_commit_clock_freq_s > 0;
    const bool commit_clock_keeper_on =
        !gap_keeper_on && cfg.dustswap_commit_clock_freq_s > 0;
    const bool have_chainlink = !events.p_chainlink.empty();
    uint64_t last_dynamic_attempt_ts = 0;
    bool have_dynamic_attempt = false;
    uint64_t last_gap_check_ts = 0;
    bool have_gap_check = false;
    uint64_t last_price_scale_change_ts = pool.last_timestamp;

    const Yb2LCosts<T> yb_2l_costs{};
    if (yb_2l_on) {
        m.yb_2l_start_collateral =
            yb_2l_actor.state().collateral;
        m.yb_2l_start_debt = yb_2l_actor.state().debt;
        m.yb_2l_start_stable_balance =
            yb_2l_actor.state().stable_balance;
    }
    const auto run_yb_2l_once = [&] (
        uint64_t ev_ts,
        const T& cex_price,
        bool& did_any_trade
    ) {
        if (!yb_2l_on) return;
        if constexpr (std::is_floating_point_v<T>) {
            const auto record_donation_reject = [&] (
                Yb2LDonationReject reject
            ) {
                switch (reject) {
                case Yb2LDonationReject::Cap:
                    ++m.yb_2l_donation_reject_cap;
                    break;
                case Yb2LDonationReject::MinMint:
                    ++m.yb_2l_donation_reject_min_mint;
                    break;
                case Yb2LDonationReject::NothingMinted:
                    ++m.yb_2l_donation_reject_nothing_minted;
                    break;
                case Yb2LDonationReject::TweakThrow:
                    ++m.yb_2l_donation_reject_tweak_throw;
                    break;
                case Yb2LDonationReject::None:
                case Yb2LDonationReject::Count:
                    break;
                }
            };
            ++m.yb_2l_attempts;
            auto actor_result = yb_2l_actor.try_fire(
                pool, cex_price, ev_ts, yb_2l_costs
            );
            if (!actor_result.fired) {
                m.yb_2l_fill_leg_aborts +=
                    actor_result.fill_leg_aborted ? 1 : 0;
                m.yb_2l_postadd_aborts +=
                    actor_result.postadd_aborted ? 1 : 0;
                record_donation_reject(actor_result.donation_reject);
                switch (actor_result.abstain) {
                case Yb2LAbstainReason::NoBandEdge:
                    ++m.yb_2l_abstain_no_band;
                    break;
                case Yb2LAbstainReason::MinFill:
                    ++m.yb_2l_abstain_min_fill;
                    break;
                case Yb2LAbstainReason::InvalidState:
                    ++m.yb_2l_abstain_invalid_state;
                    break;
                case Yb2LAbstainReason::NegativeDiscriminant:
                    ++m.yb_2l_abstain_negative_discriminant;
                    break;
                case Yb2LAbstainReason::DebtFloor:
                    ++m.yb_2l_abstain_debt_floor;
                    break;
                case Yb2LAbstainReason::StableCash:
                    ++m.yb_2l_abstain_stable_cash;
                    break;
                case Yb2LAbstainReason::UnsafeDebt:
                    ++m.yb_2l_abstain_unsafe_debt;
                    break;
                case Yb2LAbstainReason::BadFinalState:
                    ++m.yb_2l_abstain_bad_final_state;
                    break;
                case Yb2LAbstainReason::None:
                case Yb2LAbstainReason::Count:
                    break;
                }
                return;
            }

            ++m.yb_2l_fires;
            ++m.yb_2l_fires_by_direction[actor_result.direction];
            m.yb_2l_input_by_direction[actor_result.direction] +=
                actor_result.input;
            m.yb_2l_output_by_direction[actor_result.direction] +=
                actor_result.output;
            m.yb_2l_profit_by_direction[actor_result.direction] +=
                actor_result.net_profit;
            if (actor_result.levamm_price_after > T(0) &&
                actor_result.target_price > T(0)) {
                const double target_error = std::fabs(std::log(
                    static_cast<double>(
                        actor_result.levamm_price_after /
                        actor_result.target_price
                    )
                ));
                m.yb_2l_target_log_error_sum += target_error;
                m.yb_2l_target_log_error_max = std::max(
                    m.yb_2l_target_log_error_max, target_error
                );
            }
            if (m.yb_2l_first_fire_ts == 0) {
                m.yb_2l_first_fire_ts = ev_ts;
            }
            m.yb_2l_last_fire_ts = ev_ts;
            did_any_trade = true;
            m.yb_2l_fill_adds += actor_result.fill_adds;
            m.yb_2l_fill_removes += actor_result.fill_removes;
            m.yb_2l_fill_add_ps_moves +=
                actor_result.fill_add_price_scale_moves;
            m.n_rebalances += actor_result.fill_add_price_scale_moves;
            if (actor_result.fill_adds > 0 ||
                actor_result.fill_removes > 0) {
                invalidate_edge_inputs();
            }

            if (!actor_result.donation_committed) {
                ++m.yb_2l_fires_without_donation;
                record_donation_reject(actor_result.donation_reject);
                return;
            }

            ++m.yb_2l_donations;
            m.yb_2l_donation_coin0 += actor_result.donation;
            ++m.donations;
            m.donation_amounts_total[0] += actor_result.donation;
            m.donation_coin0_total += actor_result.donation;
            if (actor_result.donation_price_scale_moved) {
                ++m.yb_2l_donation_ps_moves;
                ++m.n_rebalances;
            }
            if (actor_result.virtual_price_before_donation > T(0) &&
                actor_result.virtual_price_after_donation > T(0)) {
                m.yb_2l_full_vp_log_growth += std::log(
                    static_cast<double>(
                        actor_result.virtual_price_after_donation /
                        actor_result.virtual_price_before_donation
                    )
                );
            }
            if (actor_result.xcp_profit_before_donation > T(0) &&
                actor_result.xcp_profit_after_donation > T(0)) {
                m.yb_2l_xcp_log_growth += std::log(
                    static_cast<double>(
                        actor_result.xcp_profit_after_donation /
                        actor_result.xcp_profit_before_donation
                    )
                );
            }
            m.yb_2l_burn_backfill_log_growth =
                m.yb_2l_full_vp_log_growth -
                m.yb_2l_xcp_log_growth;
            action_logger.log_yb_donation(
                ev_ts, actor_result.donation,
                actor_result.price_scale_after_donation, dcfg.apy
            );
            invalidate_edge_inputs();
        }
    };
    if (detailed_on) {
        for (size_t ev_idx = first_event_idx; ev_idx < n_events; ++ev_idx) {
            if (static_cast<size_t>(events.candle_idx[ev_idx]) >=
                candles->size()) {
                throw std::out_of_range("event.candle_idx out of range");
            }
        }
    }

    for (size_t ev_idx = first_event_idx; ev_idx < n_events; ++ev_idx) {
        const uint64_t ev_ts = events.ts[ev_idx];
        const T price_scale_at_event_start = pool.cached_price_scale;

        pool.set_block_timestamp(ev_ts);
        const T cex_price = static_cast<T>(events.p_cex[ev_idx]);
        pool.refresh_policy_context();

        sample_pre_trade(ev_ts, cex_price);
        if (donation_on && dcfg.next_ts != 0 && ev_ts >= dcfg.next_ts) {
            apply_donation(ev_idx, ev_ts);
        }

        if (!(cex_price > T(0))) {
            if (pool.cached_price_scale != price_scale_at_event_start) {
                last_price_scale_change_ts = ev_ts;
            }
            sample_apy_net_gm(ev_ts);
            continue;
        }

        bool did_any_trade = execute_arb(ev_idx, ev_ts, cex_price);
        if (yb_2l_on) {
            run_yb_2l_once(ev_ts, cex_price, did_any_trade);
        }
        if (user_swap_on && ucfg.next_ts != 0 && ev_ts >= ucfg.next_ts) {
            apply_user_swap(ev_idx, ev_ts, cex_price);
        }
        if (pool.cached_price_scale != price_scale_at_event_start) {
            last_price_scale_change_ts = ev_ts;
        }
        bool did_idle_tick = false;
        if (!did_any_trade && icfg.due(pool.last_timestamp, ev_ts)) {
            did_idle_tick = apply_idle_tick(ev_idx, ev_ts, cex_price);
            did_any_trade = did_idle_tick;
        } else if (
            !did_any_trade && dynamic_keeper_on &&
            ev_ts >= pool.last_timestamp &&
            ev_ts - pool.last_timestamp >= cfg.dustswap_dynamic_freq_s &&
            (!have_dynamic_attempt ||
             (ev_ts >= last_dynamic_attempt_ts &&
              ev_ts - last_dynamic_attempt_ts >= DYNAMIC_KEEPER_RETRY_S))
        ) {
            have_dynamic_attempt = true;
            last_dynamic_attempt_ts = ev_ts;
            did_idle_tick = apply_dynamic_idle_tick(ev_idx, ev_ts, cex_price);
            did_any_trade = did_idle_tick;
        } else if (
            !did_any_trade && policy_keeper_on &&
            (!have_gap_check ||
             (ev_ts >= last_gap_check_ts &&
              ev_ts - last_gap_check_ts >= POLICY_KEEPER_RETRY_S))
        ) {
            have_gap_check = true;
            last_gap_check_ts = ev_ts;
            did_idle_tick = apply_gap_keeper_tick(
                ev_idx,
                ev_ts,
                cex_price,
                false,
                true
            );
            if (did_idle_tick) {
                last_price_scale_change_ts = ev_ts;
                did_any_trade = true;
            }
        } else if (
            !did_any_trade && gap_keeper_on &&
            (!gap_rate_limit_on ||
             (ev_ts >= last_price_scale_change_ts &&
              ev_ts - last_price_scale_change_ts >=
                  cfg.dustswap_commit_clock_freq_s)) &&
            (!have_gap_check ||
             (ev_ts >= last_gap_check_ts &&
              ev_ts - last_gap_check_ts >= DYNAMIC_KEEPER_RETRY_S))
        ) {
            have_gap_check = true;
            last_gap_check_ts = ev_ts;
            const bool heartbeat_due =
                cfg.dustswap_dynamic_heartbeat_s > 0 &&
                ev_ts >= last_price_scale_change_ts &&
                ev_ts - last_price_scale_change_ts >=
                    cfg.dustswap_dynamic_heartbeat_s;
            did_idle_tick = apply_gap_keeper_tick(
                ev_idx,
                ev_ts,
                cex_price,
                heartbeat_due,
                false
            );
            if (did_idle_tick) {
                last_price_scale_change_ts = ev_ts;
                did_any_trade = true;
            }
        } else if (
            !did_any_trade && commit_clock_keeper_on &&
            ev_ts >= last_price_scale_change_ts &&
            ev_ts - last_price_scale_change_ts >=
                cfg.dustswap_commit_clock_freq_s &&
            (!have_dynamic_attempt ||
             (ev_ts >= last_dynamic_attempt_ts &&
              ev_ts - last_dynamic_attempt_ts >= DYNAMIC_KEEPER_RETRY_S))
        ) {
            have_dynamic_attempt = true;
            last_dynamic_attempt_ts = ev_ts;
            ++m.dynamic_keeper_commit_clock_fires;
            did_idle_tick = apply_dynamic_idle_tick(ev_idx, ev_ts, cex_price);
            if (did_idle_tick) {
                last_price_scale_change_ts = ev_ts;
                did_any_trade = true;
            }
        }
        if (detailed_on || yb_2l_on) {
            const size_t candle_idx = static_cast<size_t>(events.candle_idx[ev_idx]);
            bool detailed_row_logged = false;
            if (detailed_on) {
                detailed_row_logged = detailed_logger.log_event(
                    pool,
                    ev_ts,
                    (*candles)[candle_idx],
                    cex_price,
                    have_chainlink ? static_cast<T>(events.p_chainlink[ev_idx]) : T(0),
                    dcfg.apy,
                    m.trades,
                    m.n_rebalances,
                    enable_slippage_probes && sp.have_real[0]
                        ? sp.last_real_s01[0]
                        : std::numeric_limits<T>::quiet_NaN(),
                    enable_slippage_probes && sp.have_real[0]
                        ? sp.last_real_s10[0]
                        : std::numeric_limits<T>::quiet_NaN()
                );
            }
            if constexpr (std::is_floating_point_v<T>) {
                if (yb_2l_on) {
                    yb_2l_apy_tracker.sample(
                        pool, yb_2l_actor, ev_ts
                    );
                    const bool initialized =
                        yb_2l_apy_tracker.initialized();
                    const bool hourly_due =
                        initialized && yb_apy_gm.should_sample(ev_ts);
                    if (hourly_due || detailed_row_logged) {
                        const F growth_now = initialized
                            ? yb_2l_apy_tracker.growth()
                            : F(0);
                        if (hourly_due) {
                            yb_apy_gm.sample(ev_ts, growth_now);
                            yb_apy_gm_90d.sample(ev_ts, growth_now);
                            yb_apy_gm_30d.sample(ev_ts, growth_now);
                            yb_growth_steps.sample(ev_ts, growth_now);
                        }
                        if (detailed_row_logged) {
                            detailed_logger.annotate_last_yb(
                                initialized,
                                growth_now,
                                static_cast<F>(yb_2l_actor.state().fee),
                                m.yb_2l_fires
                            );
                            const T lp_oracle =
                                yb_2l_actor.lp_oracle(pool);
                            const T lp_fair = pool.totalSupply > T(0)
                                ? (pool.balances[0]
                                    + cex_price * pool.balances[1])
                                    / pool.totalSupply
                                : T(0);
                            detailed_logger.annotate_last_yb_position(
                                yb_2l_actor.state().stable_balance,
                                yb_2l_actor.projected_debt(ev_ts),
                                yb_2l_actor.state().collateral,
                                lp_oracle,
                                lp_fair
                            );
                        }
                    }
                }
            }
        }

        if (cex_price > T(0)) {
            const F drift_gap = static_cast<F>(
                (pool.cached_price_scale - cex_price) / cex_price
            );
            drift_ema_1d.sample(ev_ts, drift_gap);
            drift_ema_3d.sample(ev_ts, drift_gap);
            drift_stall.sample(
                ev_ts,
                static_cast<F>(pool.cached_price_scale),
                static_cast<F>(cex_price)
            );
        }
        sample_apy_net_gm(ev_ts);
    }

    if (yb_2l_on) {
        m.yb_2l_end_collateral =
            yb_2l_actor.state().collateral;
        m.yb_2l_end_debt_projected =
            yb_2l_actor.projected_debt(result.t_end);
        m.yb_2l_end_stable_balance =
            yb_2l_actor.state().stable_balance;
        const auto interest =
            yb_2l_actor.projected_interest_summary(result.t_end);
        m.yb_2l_end_pending_interest = interest.pending_interest;
        m.yb_2l_end_pending_donation = interest.pending_donation;
        m.yb_2l_interest_accrued = interest.accrued;
        m.yb_2l_interest_donated = interest.donated;
        m.yb_2l_interest_conservation_residual =
            interest.conservation_residual;
        m.yb_2l_conservation_gap_end =
            yb_2l_actor.shadow_gap_last();
        m.yb_2l_conservation_gap_max =
            yb_2l_actor.shadow_gap_max();
        m.yb_2l_conservation_checks =
            yb_2l_actor.shadow_checks();
        m.yb_2l_conservation_violations =
            yb_2l_actor.shadow_violations();
        m.yb_2l_conservation_abstains =
            yb_2l_actor.shadow_abstains();
    }
    result.apy_net_gm = apy_net_gm.value();
    result.yb_releverage_enabled = yb_2l_on;
    result.yb_releverage_fee =
        yb_2l_on ? yb_2l_actor.state().fee : T(0);
    result.yb_releverage_apy =
        yb_2l_on ? yb_2l_apy_tracker.apy() : -1.0;
    result.yb_releverage_apy_gm = yb_apy_gm.value();
    result.yb_releverage_final_growth = yb_2l_on
        ? yb_2l_apy_tracker.final_growth() : -1.0;
    result.yb_releverage_trades =
        yb_2l_on ? m.yb_2l_fires : 0;
    result.yb_releverage_gm_windows = static_cast<uint64_t>(yb_apy_gm.n_windows);
    result.yb_releverage_gm_floored_windows =
        static_cast<uint64_t>(yb_apy_gm.n_floored_windows);
    result.yb_releverage_gm_floor_share = yb_apy_gm.floor_share();

    result.lp_gm_90d_worst_window = apy_net_gm_90d.worst_window();
    result.lp_gm_90d_median_window = apy_net_gm_90d.median_window();
    result.lp_gm_90d_floor_share = apy_net_gm_90d.floor_share();
    result.yb_gm_90d_worst_window = yb_apy_gm_90d.worst_window();
    result.yb_gm_90d_median_window = yb_apy_gm_90d.median_window();
    result.yb_gm_90d_floor_share = yb_apy_gm_90d.floor_share();
    result.yb_gm_30d_cvar20 = yb_apy_gm_30d.cvar_low_window(0.20);
    result.yb_gm_30d_floor_share = yb_apy_gm_30d.floor_share();
    result.yb_growth_step_share_1d = yb_growth_steps.block_share(1, 10);
    result.yb_growth_step_share_3d = yb_growth_steps.block_share(3, 10);
    result.yb_growth_step_share_7d = yb_growth_steps.block_share(7, 5);
    result.yb_growth_top10_step_share = std::max({
        result.yb_growth_step_share_1d,
        result.yb_growth_step_share_3d,
        result.yb_growth_step_share_7d,
    });
    result.max_abs_drift_1d = drift_ema_1d.max_abs_drift();
    result.max_abs_drift_3d = drift_ema_3d.max_abs_drift();
    result.drift_3d_time_share_over_2pct = drift_ema_3d.time_share_over();
    result.drift_stall_tau_max_days = drift_stall.max_smoothed_tau_days();
    result.drift_stalled_share = drift_stall.stalled_share();

    if (yb_mode == YbMode::Passive) {
        if constexpr (std::is_floating_point_v<T>) {
            // Metrics-only shadow: re-run the exact active_2l transition over
            // the same events on a private copy of the initial pool/configs.
            // try_fire enforces state_.collateral == pool circulating supply,
            // so the shadow must evolve its own coherent pool (a fresh primary
            // copy would diverge after the first shadow LP mutation and hard-
            // abstain). Only the yb metric family is adopted; the primary
            // simulation state and its metrics are untouched.
            RunConfig<T> shadow_cfg = cfg;
            shadow_cfg.yb_mode = YbMode::Active2l;
            Pool shadow_pool = *passive_pool_initial;
            DonationCfg<T> shadow_dcfg = passive_dcfg_initial;
            UserSwapCfg<T> shadow_ucfg = passive_ucfg_initial;
            const EventLoopResult<T> shadow_result = run_event_loop(
                shadow_pool, events, costs, shadow_dcfg, icfg,
                shadow_ucfg, shadow_cfg, candles, event_start_floor_ts,
                static_cast<std::vector<Action<T>>*>(nullptr),
                static_cast<std::vector<DetailedEntry<T>>*>(nullptr)
            );
            result.yb_releverage_enabled = shadow_result.yb_releverage_enabled;
            result.yb_releverage_fee = shadow_result.yb_releverage_fee;
            result.yb_releverage_apy = shadow_result.yb_releverage_apy;
            result.yb_releverage_apy_gm = shadow_result.yb_releverage_apy_gm;
            result.yb_releverage_final_growth =
                shadow_result.yb_releverage_final_growth;
            result.yb_releverage_trades = shadow_result.yb_releverage_trades;
            result.yb_releverage_gm_windows =
                shadow_result.yb_releverage_gm_windows;
            result.yb_releverage_gm_floored_windows =
                shadow_result.yb_releverage_gm_floored_windows;
            result.yb_releverage_gm_floor_share =
                shadow_result.yb_releverage_gm_floor_share;
            result.yb_gm_90d_worst_window =
                shadow_result.yb_gm_90d_worst_window;
            result.yb_gm_90d_median_window =
                shadow_result.yb_gm_90d_median_window;
            result.yb_gm_90d_floor_share =
                shadow_result.yb_gm_90d_floor_share;
            result.yb_gm_30d_cvar20 = shadow_result.yb_gm_30d_cvar20;
            result.yb_gm_30d_floor_share =
                shadow_result.yb_gm_30d_floor_share;
            result.yb_growth_step_share_1d =
                shadow_result.yb_growth_step_share_1d;
            result.yb_growth_step_share_3d =
                shadow_result.yb_growth_step_share_3d;
            result.yb_growth_step_share_7d =
                shadow_result.yb_growth_step_share_7d;
            result.yb_growth_top10_step_share =
                shadow_result.yb_growth_top10_step_share;
        }
    }

    return result;
}

} // namespace harness
} // namespace arb
