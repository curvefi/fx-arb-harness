// Event loop for processing price events and executing arb trades
#pragma once

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <limits>
#include <optional>
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
#include "harness/yb_reference_2l.hpp"
#include "harness/pool_snapshot.hpp"
#include "harness/run_config.hpp"
#include "harness/user_swap.hpp"
#include "trading/costs.hpp"
#include "trading/arbitrageur.hpp"
#include "pools/twocrypto_fx/helpers.hpp"

namespace arb {
namespace harness {

template <typename T>
struct YbLoopState {
    using F = MetricF<T>;

    Yb2LActor<T> actor;
    YbReference2LMarket<T> reference_market;
    Yb2LApyTracker<T> apy_tracker;
    RollingGeoApy90d<F> apy_gm;
    Yb2LCosts<T> actor_costs{};
    YbReference2LCosts<T> reference_costs{};
    uint64_t last_valuation_ts{0};

};

template <bool EnableYb, bool GridCore, typename T, typename Pool>
EventLoopResult<T> run_event_loop_impl(
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
    const bool arbitrage_enabled = cfg.arbitrage_enabled;
    const bool enable_slippage_probes = cfg.enable_slippage_probes;
    const size_t detailed_interval = cfg.detailed_interval;
    const T yb_releverage_fee = cfg.yb_releverage_fee;
    const YbMode yb_mode = cfg.yb_mode;

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
    NetApyRobust90d<F> apy_net_robust_90d;
    apy_net_robust_90d.reserve_duration(result.t_end - result.t_start);
    std::optional<YbLoopState<T>> yb;
    if constexpr (EnableYb) {
        yb.emplace();
    }
    auto donation_growth_since_start = [&](uint64_t ts) -> F {
        const F elapsed_s = ts > result.t_start
            ? static_cast<F>(ts - result.t_start)
            : F(0);
        return donation_growth<F>(static_cast<F>(dcfg.apy), static_cast<F>(dcfg.freq_s), elapsed_s);
    };
    auto sample_net_apy = [&](uint64_t ts) {
        const bool legacy_due = !GridCore && apy_net_gm.should_sample(ts);
        const bool robust_due = apy_net_robust_90d.should_sample(ts);
        if (!legacy_due && !robust_due) {
            return;
        }
        const F donation_growth = donation_growth_since_start(ts);
        if (!(donation_growth > F(0))) {
            return;
        }
        const T lp_profit_growth = pool.lp_xcp_profit;
        const F net_lp_profit_growth =
            static_cast<F>(lp_profit_growth) / donation_growth;
        if (legacy_due) {
            apy_net_gm.sample(ts, net_lp_profit_growth);
        }
        if (robust_due) {
            apy_net_robust_90d.sample(ts, net_lp_profit_growth);
        }
    };
    sample_net_apy(result.t_start);

    std::array<T, SlippageProbes<T>::N_SIZES> probe_sizes_coin0{};
    if (enable_slippage_probes) {
        for (size_t k = 0; k < SlippageProbes<T>::N_SIZES; ++k) {
            probe_sizes_coin0[k] = result.tvl_start * static_cast<T>(SlippageProbes<T>::SIZE_FRACS[k]);
        }
    }

    ActionLogger<T> action_logger(out_actions);
    DetailedLogger<T> detailed_logger(out_detailed_entries, detailed_interval);
    if constexpr (EnableYb) {
        if constexpr (std::is_floating_point_v<T>) {
            if (yb_mode == YbMode::Active2l) {
                yb->actor = Yb2LActor<T>::fresh_2l(
                    pool, dcfg.apy, yb_releverage_fee, result.t_start,
                    cfg.yb_cash_multiplier
                );
            } else if (yb_mode == YbMode::Reference2l) {
                yb->reference_market = YbReference2LMarket<T>::fresh_2l(
                    pool, dcfg.apy, yb_releverage_fee, result.t_start,
                    cfg.yb_cash_multiplier
                );
            }
        } else {
            throw std::invalid_argument(
                "YieldBasis is available only on floating-point runtimes"
            );
        }
    }

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

    const bool fee_cacheable = pool.uses_native_fee_model();
    bool geometry_valid = false;
    bool fee_valid = false;
    T edge_p_now{};
    T edge_floor_scaled_p{};
    T edge_fee{};
    std::array<T, 2> edge_xp{};
    const T omf_floor = std::max(T(1) - pool.fee_lower_bound(), T(1e-12));
    auto refresh_geometry = [&]() {
        if (geometry_valid) return;
        edge_xp = pools::twocrypto_fx::pool_xp_current(pool);
        edge_p_now = pools::twocrypto_fx::MathOps<T>::get_p(
            edge_xp, pool.D, {pool.A, pool.gamma}
        ) * pool.cached_price_scale;
        edge_floor_scaled_p = omf_floor * edge_p_now;
        geometry_valid = true;
    };
    auto refresh_edge_fee = [&]() {
        refresh_geometry();
        if (fee_cacheable && fee_valid) return;
        edge_fee = pool.fee(edge_xp);
        fee_valid = true;
    };
    auto invalidate_edge_inputs = [&]() {
        geometry_valid = false;
        fee_valid = false;
    };
    uint64_t last_tw_sample_ts = 0;
    bool have_tw_sample = false;
    auto sample_pre_trade = [&](uint64_t ts, T cex_price) {
        if (have_tw_sample && ts < last_tw_sample_ts + TimeWeightedMetrics<T>::PRICE_DIFF_BUCKET_S) {
            return;
        }
        last_tw_sample_ts = ts;
        have_tw_sample = true;

        tw.template sample_price_error<GridCore>(
            ts, pool.cached_price_scale, cex_price
        );

        tw.sample_imbalance(
            ts, pool.balances[0], pool.balances[1] * cex_price
        );

        if constexpr (!GridCore) {
            refresh_edge_fee();
            tw.sample_fee(ts, edge_fee);
        }
    };

    auto apply_donation = [&](std::size_t, uint64_t ts) {
        auto don_res = make_donation_ex(pool, dcfg, ts, m);
        if (don_res.success) {
            invalidate_edge_inputs();
            action_logger.log_donation(ts, don_res, dcfg);
        }
    };

    auto apply_user_swap = [&](std::size_t, uint64_t ts, T cex_price) {
        if (try_user_swap(pool, ucfg, ts, cex_price)) {
            invalidate_edge_inputs();
        }
    };

    auto execute_arb = [&](size_t ev_idx, uint64_t ev_ts, T cex_price) -> bool {
        if (!arbitrage_enabled) {
            return false;
        }
        refresh_geometry();
        if (!(omf_floor * (cex_fee_discount * cex_price) > edge_p_now) &&
            !(edge_floor_scaled_p > cex_fee_markup * cex_price)) {
            return false;
        }
        refresh_edge_fee();
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
        if (!dec.do_trade && dec.profit < T(0)) {
            m.arb_guarded_loss_coin0 += -dec.profit;
        }
        if (!dec.do_trade) {
            return false;
        }

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

    const bool detailed_on = detailed_logger.enabled();
    const bool user_swap_on = ucfg.enabled();
    bool yb_2l_on = false;
    bool yb_reference_on = false;
    if constexpr (EnableYb) {
        yb_2l_on = yb->actor.enabled();
        yb_reference_on = yb->reference_market.enabled();
    }
    const bool yb_on = yb_2l_on || yb_reference_on;
    const bool donation_on = dcfg.enabled && !yb_on;
    const bool have_price_feed = !events.p_price_feed.empty();

    // With arbitrage enabled, skips are gated by the policy's conservative fee
    // floor. With arbitrage disabled, only scheduled observations and
    // mutations can affect results, so exact_skip may jump directly to them.
    const bool arb_exact_skip_on =
        GridCore &&
        cfg.event_cursor == EventCursor::ExactSkip &&
        arbitrage_enabled &&
        !EnableYb && !detailed_on && !action_logger.enabled() &&
        !enable_slippage_probes && !user_swap_on &&
        pool.mid_fee == pool.out_fee &&
        events.price_blocks.ready_for(n_events);
    const bool no_arb_exact_skip_on =
        cfg.event_cursor == EventCursor::ExactSkip &&
        !arbitrage_enabled &&
        !EnableYb && !detailed_on && !action_logger.enabled();
    const bool exact_skip_on = arb_exact_skip_on || no_arb_exact_skip_on;

    const auto event_passes_floor_gate = [&](double raw_price) {
        if (!(raw_price > 0.0)) return false;
        const T cex_price = static_cast<T>(raw_price);
        return
            omf_floor * (cex_fee_discount * cex_price) > edge_p_now ||
            edge_floor_scaled_p > cex_fee_markup * cex_price;
    };
    const auto block_passes_floor_gate = [&](double min_price, double max_price) {
        if (
            max_price > 0.0 &&
            omf_floor * (
                cex_fee_discount * static_cast<T>(max_price)
            ) > edge_p_now
        ) {
            return true;
        }
        return
            min_price > 0.0 &&
            edge_floor_scaled_p >
                cex_fee_markup * static_cast<T>(min_price);
    };
    const auto due_after = [](uint64_t base, uint64_t delay) {
        return delay > std::numeric_limits<uint64_t>::max() - base
            ? std::numeric_limits<uint64_t>::max()
            : base + delay;
    };
    const auto next_mandatory_ts = [&]() {
        uint64_t next = result.t_end;  // Preserve final timestamp/context.
        const auto include_due = [&](uint64_t due_ts) {
            next = std::min(next, due_ts);
        };
        if (!have_tw_sample) {
            return result.t_start;
        } else {
            include_due(due_after(
                last_tw_sample_ts,
                TimeWeightedMetrics<T>::PRICE_DIFF_BUCKET_S
            ));
        }
        if constexpr (GridCore) {
            if (!apy_net_robust_90d.have_sample) {
                return result.t_start;
            }
            include_due(due_after(
                apy_net_robust_90d.last_sample_ts,
                NetApyRobust90d<F>::SAMPLE_S
            ));
        } else {
            if (!apy_net_gm.have_sample) {
                return result.t_start;
            }
            include_due(due_after(
                apy_net_gm.last_sample_ts,
                RollingGeoApy90d<F>::SAMPLE_S
            ));
        }
        if (donation_on && dcfg.next_ts != 0) {
            include_due(dcfg.next_ts);
        }
        if (user_swap_on && ucfg.next_ts != 0) {
            include_due(ucfg.next_ts);
        }
        if (icfg.enabled()) {
            include_due(due_after(
                pool.last_timestamp,
                icfg.freq_s
            ));
        }
        return next;
    };
    const auto next_event_index = [&](size_t start) {
        if (!exact_skip_on || start >= n_events) return start;

        const uint64_t mandatory_ts = next_mandatory_ts();
        if (events.ts[start] >= mandatory_ts) return start;
        if (no_arb_exact_skip_on) {
            return static_cast<size_t>(std::lower_bound(
                events.ts.begin() + static_cast<std::ptrdiff_t>(start),
                events.ts.end(),
                mandatory_ts
            ) - events.ts.begin());
        }
        refresh_geometry();
        const auto finish_jump = [&](size_t destination) {
#if defined(ARB_VALIDATE_EVENT_JUMPS)
            for (size_t index = start; index < destination; ++index) {
                if (events.ts[index] >= mandatory_ts) {
                    throw std::logic_error(
                        "exact event cursor skipped a mandatory event"
                    );
                }
                if (event_passes_floor_gate(events.p_cex[index])) {
                    throw std::logic_error(
                        "exact event cursor skipped an arb-floor survivor"
                    );
                }
            }
#endif
            return destination;
        };

        size_t cursor = start;
        constexpr size_t block_size = PriceBlockIndex::BLOCK_SIZE;
        while (cursor < n_events) {
            const size_t block = cursor / block_size;
            const size_t block_begin = block * block_size;
            const size_t block_end = std::min(
                block_begin + block_size, n_events
            );
            if (
                cursor == block_begin &&
                events.ts[block_end - 1] < mandatory_ts &&
                !block_passes_floor_gate(
                    events.price_blocks.min_positive[block],
                    events.price_blocks.max_positive[block]
                )
            ) {
                cursor = block_end;
                continue;
            }
            for (; cursor < block_end; ++cursor) {
                if (events.ts[cursor] >= mandatory_ts) {
                    return finish_jump(cursor);
                }
                if (event_passes_floor_gate(events.p_cex[cursor])) {
                    return finish_jump(cursor);
                }
            }
        }
        return finish_jump(n_events);
    };

    const auto run_yb_2l_once = [&] (
        uint64_t ev_ts,
        const T& cex_price,
        bool& did_any_trade
    ) {
        auto& yb_2l_actor = yb->actor;
        const auto& yb_2l_costs = yb->actor_costs;
        if (!yb_2l_on) return;
        if constexpr (std::is_floating_point_v<T>) {
            auto actor_result = yb_2l_actor.try_fire(
                pool, cex_price, ev_ts, yb_2l_costs
            );
            if (!actor_result.fired) return;

            ++m.yb_2l_fires;
            did_any_trade = true;
            m.n_rebalances += actor_result.fill_add_price_scale_moves;
            if (actor_result.fill_adds > 0 ||
                actor_result.fill_removes > 0) {
                invalidate_edge_inputs();
            }

            if (!actor_result.donation_committed) return;

            ++m.donations;
            m.donation_amounts_total[0] += actor_result.donation;
            m.donation_coin0_total += actor_result.donation;
            if (actor_result.donation_price_scale_moved) {
                ++m.n_rebalances;
            }
            action_logger.log_yb_donation(
                ev_ts, actor_result.donation,
                actor_result.price_scale_after_donation, dcfg.apy
            );
            invalidate_edge_inputs();
        }
    };
    const auto run_yb_reference_once = [&] (
        uint64_t ev_ts,
        const T& cex_price,
        bool& did_any_trade
    ) {
        auto& yb_reference_market = yb->reference_market;
        const auto& yb_reference_costs = yb->reference_costs;
        if (!yb_reference_on) return;
        if constexpr (std::is_floating_point_v<T>) {
            const T price_scale_before = pool.cached_price_scale;
            auto route = yb_reference_market.execute_best(
                pool, cex_price, ev_ts, yb_reference_costs, true
            );
            if (!route.committed) return;

            ++m.yb_2l_fires;

            const T before_donation = route.direction == 1
                ? route.price_scale_after_add : price_scale_before;
            if (route.emitted_add &&
                route.price_scale_after_add != price_scale_before) {
                ++m.n_rebalances;
            }
            if (route.emitted_donation &&
                route.price_scale_after_donation != before_donation) {
                ++m.n_rebalances;
            }

            if (route.emitted_donation) {
                ++m.donations;
                m.donation_amounts_total[0] += route.donation;
                m.donation_coin0_total += route.donation;
                action_logger.log_yb_donation(
                    ev_ts, route.donation,
                    route.price_scale_after_donation, dcfg.apy
                );
            }
            did_any_trade = true;
            invalidate_edge_inputs();
        }
    };
    const auto sample_yb_report = [&] (
        const auto& market,
        uint64_t ts,
        const T& cex_price,
        bool detailed_row_logged
    ) {
        const bool hourly_due = yb->apy_gm.should_sample(ts);
        if (!hourly_due && !detailed_row_logged) return;

        yb->apy_tracker.sample(pool, market, ts);
        yb->last_valuation_ts = ts;
        const bool initialized = yb->apy_tracker.initialized();
        const F growth_now = initialized
            ? yb->apy_tracker.growth()
            : F(0);

        if (hourly_due && initialized) {
            yb->apy_gm.sample(ts, growth_now);
        }
        if (!detailed_row_logged) return;

        detailed_logger.annotate_last_yb(
            initialized,
            growth_now,
            static_cast<F>(market.state().fee),
            m.yb_2l_fires
        );
        const T lp_oracle = market.lp_oracle(pool);
        const T lp_fair = pool.totalSupply > T(0)
            ? (pool.balances[0] + cex_price * pool.balances[1]) /
                pool.totalSupply
            : T(0);
        detailed_logger.annotate_last_yb_position(
            market.state().stable_balance,
            market.projected_debt(ts),
            market.state().collateral,
            lp_oracle,
            lp_fair
        );
    };
    if (detailed_on) {
        for (size_t ev_idx = first_event_idx; ev_idx < n_events; ++ev_idx) {
            if (static_cast<size_t>(events.candle_idx[ev_idx]) >=
                candles->size()) {
                throw std::out_of_range("event.candle_idx out of range");
            }
        }
    }

    size_t ev_idx = first_event_idx;
    while (ev_idx < n_events) {
        const uint64_t ev_ts = events.ts[ev_idx];

        pool.set_block_timestamp(ev_ts);
        const T cex_price = static_cast<T>(events.p_cex[ev_idx]);
        if (have_price_feed) {
            pool.refresh_policy_context(
                static_cast<T>(events.p_price_feed[ev_idx]),
                events.price_feed_ts[ev_idx]
            );
        } else {
            pool.refresh_policy_context();
        }

        sample_pre_trade(ev_ts, cex_price);
        if (donation_on && dcfg.next_ts != 0 && ev_ts >= dcfg.next_ts) {
            apply_donation(ev_idx, ev_ts);
        }

        if (!(cex_price > T(0))) {
            sample_net_apy(ev_ts);
            ev_idx = next_event_index(ev_idx + 1);
            continue;
        }

        bool did_any_trade = execute_arb(ev_idx, ev_ts, cex_price);
        if constexpr (EnableYb) {
            if (yb_2l_on) {
                run_yb_2l_once(ev_ts, cex_price, did_any_trade);
            } else if (yb_reference_on) {
                run_yb_reference_once(ev_ts, cex_price, did_any_trade);
            }
        }
        if (user_swap_on && ucfg.next_ts != 0 && ev_ts >= ucfg.next_ts) {
            apply_user_swap(ev_idx, ev_ts, cex_price);
        }
        bool did_idle_tick = false;
        if (!did_any_trade && icfg.due(pool.last_timestamp, ev_ts)) {
            did_idle_tick = apply_idle_tick(ev_idx, ev_ts, cex_price);
            did_any_trade = did_idle_tick;
        }
        bool detailed_row_logged = false;
        if (detailed_on) {
            const size_t candle_idx =
                static_cast<size_t>(events.candle_idx[ev_idx]);
            detailed_row_logged = detailed_logger.log_event(
                pool,
                ev_ts,
                (*candles)[candle_idx],
                cex_price,
                have_price_feed
                    ? static_cast<T>(events.p_price_feed[ev_idx])
                    : T(0),
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
        if constexpr (EnableYb) {
            if constexpr (std::is_floating_point_v<T>) {
                if (yb_2l_on) {
                    sample_yb_report(
                        yb->actor, ev_ts, cex_price, detailed_row_logged
                    );
                } else if (yb_reference_on) {
                    sample_yb_report(
                        yb->reference_market, ev_ts, cex_price,
                        detailed_row_logged
                    );
                }
            }
        }

        sample_net_apy(ev_ts);
        ev_idx = next_event_index(ev_idx + 1);
    }

    result.apy_net_gm = apy_net_gm.value();
    result.apy_net_robust_90d = apy_net_robust_90d.value();
    if constexpr (EnableYb) {
        // Raw YB APY is an endpoint metric. If the last event was between
        // hourly reports, mark it once without adding another GM window.
        if constexpr (std::is_floating_point_v<T>) {
            const auto sample_yb_endpoint = [&](const auto& market) {
                if (yb->last_valuation_ts == result.t_end) return;
                yb->apy_tracker.sample(pool, market, result.t_end);
                yb->last_valuation_ts = result.t_end;
            };
            if (yb_2l_on) {
                sample_yb_endpoint(yb->actor);
            } else if (yb_reference_on) {
                sample_yb_endpoint(yb->reference_market);
            }
        }

        result.yb_releverage_fee = yb_2l_on
            ? yb->actor.state().fee
            : (yb_reference_on ? yb->reference_market.state().fee : T(0));
        result.yb_releverage_apy =
            yb_on ? yb->apy_tracker.apy() : -1.0;
        result.yb_releverage_apy_gm = yb->apy_gm.value();
        result.yb_releverage_final_growth = yb_on
            ? yb->apy_tracker.final_growth() : -1.0;
        result.yb_releverage_trades = yb_on ? m.yb_2l_fires : 0;
        result.yb_releverage_gm_windows =
            static_cast<uint64_t>(yb->apy_gm.n_windows);
        result.yb_releverage_gm_floored_windows =
            static_cast<uint64_t>(yb->apy_gm.n_floored_windows);
        result.yb_releverage_gm_floor_share = yb->apy_gm.floor_share();
    }
    return result;
}

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
    if (cfg.yb_mode == YbMode::Off) {
        if (cfg.metric_profile == MetricProfile::GridCore) {
            return run_event_loop_impl<false, true>(
                pool, events, costs, dcfg, icfg, ucfg, cfg, candles,
                event_start_floor_ts, out_actions, out_detailed_entries
            );
        }
        return run_event_loop_impl<false, false>(
            pool, events, costs, dcfg, icfg, ucfg, cfg, candles,
            event_start_floor_ts, out_actions, out_detailed_entries
        );
    }
    if (cfg.metric_profile == MetricProfile::GridCore) {
        throw std::invalid_argument(
            "grid_core metric profile does not support YieldBasis"
        );
    }
    return run_event_loop_impl<true, false>(
        pool, events, costs, dcfg, icfg, ucfg, cfg, candles,
        event_start_floor_ts, out_actions, out_detailed_entries
    );
}

} // namespace harness
} // namespace arb
