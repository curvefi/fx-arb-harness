// Event loop for processing price events and executing arb trades
#pragma once
#include "harness/yb_external_equity.hpp"

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
#include "harness/pool_snapshot.hpp"
#include "harness/run_config.hpp"
#include "harness/report_window.hpp"
#include "harness/user_swap.hpp"
#include "trading/costs.hpp"
#include "trading/arbitrageur.hpp"
#include "pools/twocrypto_fx/helpers.hpp"

namespace arb {
namespace harness {

// A fixed SplitMix64 stream indexed by the original event ordinal gives every
// candidate the same draws, independent of trade count, workers, and skipping.
inline bool arb_brings_report(size_t event_index, double rate) {
    uint64_t draw = static_cast<uint64_t>(event_index) + 0x9e3779b97f4a7c15ULL;
    draw = (draw ^ (draw >> 30)) * 0xbf58476d1ce4e5b9ULL;
    draw = (draw ^ (draw >> 27)) * 0x94d049bb133111ebULL;
    draw ^= draw >> 31;
    return static_cast<double>(draw >> 11) * 0x1.0p-53 < rate;
}

// Independent deterministic draw for the one-report availability model.
// The original event ordinal keeps choices stable across candidates and cursors.
inline uint64_t arb_report_sample_draw(size_t event_index) {
    uint64_t draw = static_cast<uint64_t>(event_index) + 0xd1b54a32d192ed03ULL;
    draw = (draw ^ (draw >> 30)) * 0xbf58476d1ce4e5b9ULL;
    draw = (draw ^ (draw >> 27)) * 0x94d049bb133111ebULL;
    return draw ^ (draw >> 31);
}

template <typename T>
struct YbLoopState {
    using F = MetricF<T>;

    Yb2LActor<T> actor;
    Yb2LApyTracker<T> apy_tracker;
    YbExternalEquity external_equity;
    RollingGeoApy90d<F> apy_gm;
    // Reuse the hourly valuation and sampling clock; bounded queues are only
    // advanced once per hour, never scanned across the simulation history.
    RollingGeoApyWindow<F> apy_gm30{30ULL * 86400, RollingGeoApy90d<F>::FLOOR_APY};
    RollingGeoApyWindow<F> apy_gm60{60ULL * 86400, RollingGeoApy90d<F>::FLOOR_APY};
    Yb2LCosts<T> actor_costs{};
    uint64_t last_valuation_ts{0};
    SampledLogVariation<F> price_scale_variation;

};

template <bool EnableYb, typename T, typename Pool>
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
    if (!(cfg.arb_report_rate >= T(0) && cfg.arb_report_rate <= T(1))) {
        throw std::invalid_argument("arb_report_rate must be in [0, 1]");
    }
    const double report_max_age_s = static_cast<double>(cfg.arb_report_max_age_s);
    if (!std::isfinite(report_max_age_s) || report_max_age_s < 0)
        throw std::invalid_argument("arb_report_max_age_s must be finite and nonnegative");
    const double count_value = static_cast<double>(cfg.arb_report_count);
    if (!std::isfinite(count_value) || count_value < 0 || count_value > 1024 || std::floor(count_value) != count_value)
        throw std::invalid_argument("arb_report_count must be an integer in [0, 1024]");
    if (count_value > 0 && report_max_age_s > 0)
        throw std::invalid_argument("report count and time window are mutually exclusive");
    const size_t report_count = static_cast<size_t>(count_value);
    const double early_stop_threshold = cfg.early_stop_max_7d_rel_price_diff;
    if (!std::isfinite(early_stop_threshold) || early_stop_threshold < 0)
        throw std::invalid_argument("early_stop_max_7d_rel_price_diff must be finite and nonnegative");
    const bool early_stop_on = early_stop_threshold > 0;
    const double random_count_value = static_cast<double>(cfg.arb_report_random_count);
    if (!std::isfinite(random_count_value) || random_count_value < 0 ||
        random_count_value > 6 || std::floor(random_count_value) != random_count_value)
        throw std::invalid_argument("arb_report_random_count must be an integer in [0, 6]");
    const size_t random_count = static_cast<size_t>(random_count_value);
    const double offset_value = static_cast<double>(cfg.arb_report_offset);
    if (!std::isfinite(offset_value) || offset_value < -1 || offset_value > 1023 || std::floor(offset_value) != offset_value)
        throw std::invalid_argument("arb_report_offset must be -1 or an integer in [0, 1023]");
    const bool fixed_report = offset_value >= 0;
    if (random_count > 0 && (fixed_report || report_count > 0 || report_max_age_s > 0))
        throw std::invalid_argument("random report count, offset, count, and time window are mutually exclusive");
    if (fixed_report && (report_count > 0 || report_max_age_s > 0))
        throw std::invalid_argument("report offset, count, and time window are mutually exclusive");
    const bool search_reports = fixed_report || report_count > 0 || report_max_age_s > 0 || random_count > 0;
    if (search_reports && (!pool.uses_swap_reports() || events.report_ts.empty()))
        throw std::invalid_argument("report selection requires a swap-report policy and price_feed_path");
    if (events.report_ts.size() != events.report_prices.size())
        throw std::invalid_argument("report timestamp and price counts differ");
    // Zero selects the latest independent observation, with its true age.
    // Positive values select the best admissible submission in that window.
    pool.policy.research.report_max_age_s = report_max_age_s == 0 && !events.report_ts.empty()
        ? std::numeric_limits<double>::max() : report_max_age_s;
    ReportWindow report_window(events, report_max_age_s, report_count);
    const T max_swap_frac = cfg.max_swap_frac;
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
    const auto initial_balances = pool.balances;
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
        const bool legacy_due = apy_net_gm.should_sample(ts);
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
                yb->actor_costs.min_profit_coin0 = cfg.yb_min_net_profit_coin0;
                yb->actor_costs.execution_bps = {cfg.yb_execution_bps, cfg.yb_execution_bps};
                yb->actor = cfg.yb_initial_state
                    ? Yb2LActor<T>::from_state(*cfg.yb_initial_state)
                    : Yb2LActor<T>::fresh_2l(
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

    if constexpr (EnableYb && std::is_floating_point_v<T>) {
        const T initial_mark = static_cast<T>(events.p_cex[first_event_idx]);
        if (yb->actor.enabled()) yb->external_equity.sample(pool, yb->actor, result.t_start, initial_mark);
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

    // Fee-model facts follow the pool's configuration, which can change:
    // refreshed with the geometry after every pool mutation.
    bool fee_cacheable = pool.uses_native_fee_model();
    bool geometry_valid = false;
    bool fee_valid = false;
    T edge_p_now{};
    T edge_floor_scaled_p{};
    T edge_fee{};
    std::array<T, 2> edge_xp{};
    T omf_floor = std::max(T(1) - pool.fee_lower_bound(), T(1e-12));
    auto refresh_geometry = [&]() {
        if (geometry_valid) return;
        fee_cacheable = pool.uses_native_fee_model();
        omf_floor = std::max(T(1) - pool.fee_lower_bound(), T(1e-12));
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
    // Cached YB no-trade price band: valid until the pool or the actor changes.
    typename Yb2LActor<T>::NoTradeBand yb_band{};
    bool yb_band_ready = false;
    auto invalidate_edge_inputs = [&]() {
        geometry_valid = false;
        fee_valid = false;
        yb_band_ready = false;
    };
    uint64_t last_tw_sample_ts = 0;
    bool have_tw_sample = false;
    auto sample_pre_trade = [&](uint64_t ts, T cex_price) {
        if (have_tw_sample && ts < last_tw_sample_ts + TimeWeightedMetrics<T>::PRICE_DIFF_BUCKET_S) {
            return;
        }
        last_tw_sample_ts = ts;
        have_tw_sample = true;

        tw.sample_price_error(
            ts, pool.cached_price_scale, cex_price
        );

        tw.sample_imbalance(
            ts, pool.balances[0], pool.balances[1] * cex_price
        );

        refresh_edge_fee();
        tw.sample_fee(ts, edge_fee);
    };

    auto apply_donation = [&](uint64_t ts, T cex_price) {
        auto don_res = make_donation_ex(pool, dcfg, ts, m);
        if (don_res.success) {
            invalidate_edge_inputs();
            if (enable_slippage_probes) {
                sample_slippage_probes(ts, cex_price);
            }
            action_logger.log_donation(ts, don_res, dcfg);
        }
    };

    auto apply_user_swap = [&](uint64_t ts, T cex_price) {
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
        const auto fill = try_user_swap(pool, ucfg, ts, cex_price);
        if (!fill) return;

        invalidate_edge_inputs();
        if (differs_rel(pool.cached_price_scale, ps_before)) {
            ++m.n_rebalances;
        }
        if (enable_slippage_probes) {
            sample_slippage_probes(ts, cex_price);
        }
        if (log_actions) {
            action_logger.log_exchange(
                ts, static_cast<int>(fill->i), static_cast<int>(fill->j),
                fill->dx, fill->dy_after_fee, fill->fee_tokens, T(0),
                cex_price, p_pool_before, oracle_before, ps_before,
                last_ts_before, lp_before, xcp_profit_before, vp_before,
                pool, true
            );
        }
    };

    // One report selection for sizing and for the fast cursor's skip test.
    const auto visit_offered_reports = [&](size_t ev_idx, uint64_t ev_ts, const auto& visit) {
        if (fixed_report) {
            report_window.at_offset(ev_ts, static_cast<size_t>(offset_value), visit, ev_idx);
        } else if (random_count > 0) {
            report_window.sample_last(ev_ts, random_count,
                arb_report_sample_draw(ev_idx), visit, ev_idx);
        } else {
            report_window.each(ev_ts, visit, ev_idx);
        }
    };

    // Whether the event price clears the pool's fee floor in either direction: the gate of per-event sizing.
    const auto native_may_trade = [&](T cex_price) {
        refresh_geometry();
        return cex_price > T(0) && (omf_floor * (cex_fee_discount * cex_price) > edge_p_now ||
                                    edge_floor_scaled_p > cex_fee_markup * cex_price);
    };

    // Commit a previewed native swap of dx coin i at the current research context and record it.
    const auto commit_swap = [&](uint64_t ev_ts, size_t i, T dx, T dy_after_fee, T fee_tokens, T profit,
                                 T notional_coin0, T cex_price, bool offered_report) -> bool {
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
        std::array<T, 3> res{};
        try {  // a reverting transition leaves the pool unchanged
            res = pool.exchange_from_preview(i, 1 - i, dx, dy_after_fee, fee_tokens);
        } catch (...) {
            return false;
        }
        m.trades += 1;
        if (pool.uses_swap_reports()) {
            if (offered_report) ++m.arb_offered_report_trades;
            else ++m.arb_withheld_report_trades;
        }
        m.notional += notional_coin0;
        m.lp_fee_coin0 += (i == 0 ? res[1] * cex_price : res[1]);
        m.arb_pnl_coin0 += profit;
        if (differs_rel(pool.cached_price_scale, ps_before)) m.n_rebalances += 1;
        if (enable_slippage_probes) sample_slippage_probes(ev_ts, cex_price);
        if (log_actions) {
            action_logger.log_exchange(ev_ts, static_cast<int>(i), static_cast<int>(1 - i), dx, res[0], res[1],
                                       profit, cex_price, p_pool_before, oracle_before, ps_before,
                                       last_ts_before, lp_before, xcp_profit_before, vp_before, pool);
        }
        return true;
    };

    auto execute_arb = [&] (
        size_t ev_idx,
        uint64_t ev_ts,
        T cex_price
    ) -> bool {
        if (!native_may_trade(cex_price)) return false;
        refresh_edge_fee();
        if (costs.entry_edge_bps > T(0)) {
            // Enter only when the first unit's fee-inclusive edge against the event price clears the threshold.
            const T keep = T(1) - edge_fee;
            const T edge = std::max(keep * cex_price / edge_p_now, keep * edge_p_now / cex_price);
            if (!(edge > std::exp(costs.entry_edge_bps / T(10000)))) return false;
        }
        // Only research context changes while comparing actions. Pool reserves
        // and policy memory are committed once below.
        trading::SwapPreviewCache<T> previews;
        // Single-report runs rarely reuse sizes: avoid memoization overhead.
        auto* shared_previews = search_reports && !fixed_report && random_count == 0 && report_count != 1 ? &previews : nullptr;
        // Fixed costs of a swap at the current context: gas, plus report_coin0 when it offers a report.
        const auto action_costs = [&] {
            auto out = costs;
            if (pool.uses_swap_reports() && pool.policy.research.price_feed > T(0)) out.gas_coin0 += costs.report_coin0;
            return out;
        };
        const auto size_trade = [&](bool trade_only) {
            fee_valid = false;
            refresh_edge_fee();
            return trading::decide_trade(
                pool, cex_price, action_costs(), min_swap_frac, max_swap_frac,
                cex_fee_discount, cex_fee_markup, &edge_p_now, &edge_fee, &edge_xp, shared_previews, trade_only);
        };
        auto winning_context = pool.policy.research;
        auto dec = size_trade(false);
        // Alternatives replace dec only with a trade: their losses are unused.
        const auto consider = [&] {
            auto candidate = size_trade(true);
            if (candidate.do_trade && (!dec.do_trade || candidate.profit > dec.profit)) {
                dec = candidate;
                winning_context = pool.policy.research;
            }
        };
        if (search_reports && arb_brings_report(ev_idx, static_cast<double>(cfg.arb_report_rate))) {
            visit_offered_reports(ev_idx, ev_ts, [&](double price, double timestamp) {
                pool.refresh_policy_context(static_cast<T>(price), timestamp);
                consider();
            });
        } else if (pool.uses_cached_reports() && winning_context.price_feed > T(0)) {
            // Latest-report delivery is optional too: compare withholding it.
            pool.clear_policy_price_feed();
            consider();
        }
        pool.policy.research = winning_context;
        fee_valid = false;
        if (!dec.do_trade && dec.profit < T(0)) {
            m.arb_guarded_loss_coin0 += -dec.profit;
        }
        if (!dec.do_trade) {
            return false;
        }

        if (!commit_swap(ev_ts, static_cast<size_t>(dec.i), dec.dx, dec.dy_after_fee, dec.fee_tokens,
                         dec.profit, dec.notional_coin0, cex_price, winning_context.price_feed > T(0)))
            return false;
        return true;
    };

    auto apply_idle_tick = [&](uint64_t ts, T cex_price) -> bool {
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
    if constexpr (EnableYb) {
        yb_2l_on = yb->actor.enabled();
    }
    const bool yb_2l_trades = yb_2l_on && cfg.yb_arb != YbArb::None;
    const bool fast_skip = cfg.event_cursor == EventCursor::FastSkip;
    const bool donation_on = dcfg.enabled && !yb_2l_on;
    const bool have_price_feed = !events.p_price_feed.empty();
    const bool swap_reports = pool.uses_swap_reports();

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
        if (!apy_net_robust_90d.have_sample) {
            return result.t_start;
        }
        include_due(due_after(
            apy_net_robust_90d.last_sample_ts,
            NetApyRobust90d<F>::SAMPLE_S
        ));
        if (!apy_net_gm.have_sample) {
            return result.t_start;
        }
        include_due(due_after(
            apy_net_gm.last_sample_ts,
            RollingGeoApy90d<F>::SAMPLE_S
        ));
        if constexpr (EnableYb) {
            if (yb_2l_on) {
                if (!yb->apy_gm.have_sample) return result.t_start;
                include_due(due_after(yb->apy_gm.last_sample_ts, RollingGeoApy90d<F>::SAMPLE_S));
            }
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
    // Indexed fast cursor: an event is skipped only when neither its event
    // price nor the active YB gate could act,
    // tested exactly as execute_arb and the actor would, so skipping it
    // changes nothing.
    // Offered reports lower fees only through the policy's profit bound.
    const bool report_skip = swap_reports && search_reports;
    const auto reported_sizing_may_act = [&](size_t index, uint64_t ts, T bid, T ask,
                                             bool may_sell0, bool may_sell1) {
        // execute_arb sizes withholding first, with the feed cleared. Stop
        // wherever it would size: an unprofitable result is still a metric.
        auto context = pool.policy.research;
        context.block_timestamp = ts;
        context.price_oracle = pool.cached_price_oracle;
        context.price_feed = T(0);
        context.price_feed_timestamp = 0;
        const T global_floor = pool.fee_lower_bound();
        const T floor01 = pool.context_fee_lower_bound(context, 0);
        const T floor10 = pool.context_fee_lower_bound(context, 1);
        if (!(floor01 > global_floor || floor10 > global_floor) ||
            std::max(T(1) - floor01, T(1e-12)) * (cex_fee_discount * bid) > edge_p_now ||
            (ask > T(0) && std::max(T(1) - floor10, T(1e-12)) * edge_p_now > cex_fee_markup * ask))
            return true;
        // Offered reports only replace withholding with a profitable trade.
        if (!arb_brings_report(index, static_cast<double>(cfg.arb_report_rate))) return false;
        bool may = false;
        visit_offered_reports(index, ts, [&](double price, double timestamp) {
            if (may) return;
            // Leave invalid reports to set_price_feed, which rejects them.
            if (!(price > 0.0) || !std::isfinite(timestamp) || timestamp < 0 ||
                timestamp > static_cast<double>(ts)) {
                may = true;
                return;
            }
            context.price_feed = static_cast<T>(price);
            context.price_feed_timestamp = timestamp;
            may = (may_sell0 && pool.context_may_profit(context, 0, edge_p_now, cex_fee_discount * bid)) ||
                (may_sell1 && pool.context_may_profit(context, 1, edge_p_now, cex_fee_markup * ask));
        });
        return may;
    };
    // Exactly may_trade: inside the cached band the actor provably abstains.
    const auto yb_may_trade = [&](const T& mid, uint64_t ts) {
        if constexpr (EnableYb) {
            if (!yb_band_ready || ts < yb_band.from || ts > yb_band.to) {
                yb_band = yb->actor.no_trade_band(pool, mid, ts, ts + 3600, yb->actor_costs);
                yb_band_ready = true;
            }
            if (yb_band.valid && ts >= yb_band.from && ts <= yb_band.to && mid >= yb_band.lo && mid <= yb_band.hi)
                return false;
            return yb->actor.may_trade(pool, mid, ts, yb->actor_costs);
        } else {
            return false;
        }
    };
    const auto indexed_may_act = [&](size_t index, uint64_t ts) {
        const double bid = events.p_cex[index], ask = bid;
        refresh_geometry();
        const T best_bid = static_cast<T>(bid), best_ask = static_cast<T>(ask);
        if ((omf_floor * (cex_fee_discount * best_bid) > edge_p_now ||
             (best_ask > T(0) && edge_floor_scaled_p > cex_fee_markup * best_ask)) &&
            (!report_skip || reported_sizing_may_act(index, ts, best_bid, best_ask,
                // decide_trade sizes only a direction clearing the global floor;
                // the slack covers its differently rounded ratio test.
                omf_floor * (cex_fee_discount * best_bid) > edge_p_now * T(1 - 1e-12),
                best_ask > T(0) && edge_floor_scaled_p > cex_fee_markup * best_ask * T(1 - 1e-12))))
            return true;
        if constexpr (EnableYb) {
            if (yb_2l_trades && yb_may_trade(static_cast<T>(events.p_cex[index]), ts)) return true;
        }
        return false;
    };
    const auto next_event_index = [&](size_t start) {
        if (!fast_skip) return start;
        // First event at/after any deadline must reach the loop, even without
        // profitable actors. d3600 stays tied to the last committed pool touch.
        const uint64_t mandatory = next_mandatory_ts();
        for (; start < n_events; ++start) {
            const auto ts = events.ts[start];
            if (ts >= mandatory || indexed_may_act(start, ts)) break;
        }
        return start;
    };

    // One YB actor decision.
    const auto run_yb_2l_once = [&] (
        uint64_t ev_ts,
        const T& cex_price,
        bool& did_any_trade
    ) {
        auto& yb_2l_actor = yb->actor;
        const auto& yb_2l_costs = yb->actor_costs;
        if (!yb_2l_trades) return;
        if constexpr (std::is_floating_point_v<T>) {
            if (fast_skip && !yb_may_trade(cex_price, ev_ts)) return;
            auto actor_result = yb_2l_actor.try_fire(
                pool, cex_price, ev_ts, yb_2l_costs
            );
            if (actor_result.fired) yb_band_ready = false;  // a refused route leaves the pool and ledger as they were
            if (!actor_result.fired) return;

            ++m.yb_2l_fires;
            m.yb_levamm_profit_coin0 += actor_result.net_profit;
            did_any_trade = true;
            action_logger.log_injected(ev_ts, m.yb_2l_fires - 1, {actor_result.input, actor_result.output}, pool);
            action_logger.annotate_last_yb(static_cast<uint8_t>(actor_result.direction),
                yb_2l_actor.state().collateral, yb_2l_actor.projected_debt(ev_ts),
                yb_2l_actor.state().stable_balance, yb_2l_actor.levamm_price(pool, ev_ts),
                actor_result.donation_committed ? actor_result.donation : T(0));
            m.n_rebalances += actor_result.fill_add_price_scale_moves;
            if (actor_result.fill_adds > 0 ||
                actor_result.fill_removes > 0) {
                invalidate_edge_inputs();
            }

            if (!actor_result.donation_committed) return;

            ++m.donations;
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
    const auto sample_yb_report = [&] (
        const auto& market,
        uint64_t ts,
        const T& cex_price,
        bool detailed_row_logged
    ) {
        const bool hourly_due = yb->apy_gm.should_sample(ts);
        if (!hourly_due && !detailed_row_logged) return;

        if (hourly_due) yb->external_equity.sample(pool, market, ts, cex_price);
        yb->apy_tracker.sample(pool, market, ts);
        yb->last_valuation_ts = ts;
        const bool initialized = yb->apy_tracker.initialized();
        const F growth_now = initialized
            ? yb->apy_tracker.growth()
            : F(0);

        if (hourly_due && initialized) {
            yb->price_scale_variation.sample(ts, static_cast<F>(pool.cached_price_scale));
            yb->apy_gm.sample(ts, growth_now);
            yb->apy_gm30.sample(ts, growth_now);
            yb->apy_gm60.sample(ts, growth_now);
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
    size_t final_event_idx = n_events - 1;
    while (ev_idx < n_events) {
        const uint64_t ev_ts = events.ts[ev_idx];
        pool.set_block_timestamp(ev_ts);
        const T cex_price = static_cast<T>(events.p_cex[ev_idx]);
        const bool arb_may_trade = native_may_trade(cex_price);
        if (swap_reports) {
            pool.clear_policy_price_feed();
            pool.refresh_policy_context();
        } else if (have_price_feed) {
            pool.refresh_policy_context(static_cast<T>(events.p_price_feed[ev_idx]),
                                        events.price_feed_ts[ev_idx]);
        } else {
            pool.refresh_policy_context();
        }

        sample_pre_trade(ev_ts, cex_price);
        // The 7-day maximum never falls: past the threshold the result's
        // divergence mask is settled, so the remaining history is skipped.
        if (early_stop_on && tw.have_7d_rel_abs &&
            static_cast<double>(tw.max_7d_rel_abs) > early_stop_threshold) {
            result.early_stop_ts = ev_ts;
            result.t_end = ev_ts;
            final_event_idx = ev_idx;
            break;
        }
        if (donation_on && dcfg.next_ts != 0 && ev_ts >= dcfg.next_ts) {
            apply_donation(ev_ts, cex_price);
        }

        if (!(cex_price > T(0))) {
            sample_net_apy(ev_ts);
            ev_idx = next_event_index(ev_idx + 1);
            continue;
        }

        // Expose a report before sizing. A cache-capable policy also compares
        // withholding it; other swap-report policies retain one-shot semantics.
        if (swap_reports && arb_brings_report(
                ev_idx, static_cast<double>(cfg.arb_report_rate))) {
            if (search_reports) {
                // Start from withholding the report. execute_arb compares the
                // fully sized alternatives before any state is committed.
            } else {
                pool.refresh_policy_context(
                    have_price_feed ? static_cast<T>(events.p_price_feed[ev_idx]) : cex_price,
                    have_price_feed ? events.price_feed_ts[ev_idx] : ev_ts);
            }
        }
        // Reports affect policy quotes, not pool geometry. Actual reserve,
        // invariant and price-scale mutations invalidate both caches below.
        if (swap_reports) fee_valid = false;
        bool did_any_trade = arb_may_trade && execute_arb(ev_idx, ev_ts, cex_price);
        if (swap_reports) {
            pool.clear_policy_price_feed();
            fee_valid = false;
        }
        if constexpr (EnableYb) {
            if (yb_2l_on) {
                run_yb_2l_once(ev_ts, cex_price, did_any_trade);
            }
        }
        if (user_swap_on && ucfg.next_ts != 0 && ev_ts >= ucfg.next_ts) {
            apply_user_swap(ev_ts, cex_price);
        }
        bool did_idle_tick = false;
        if (!did_any_trade && icfg.due(pool.last_timestamp, ev_ts)) {
            did_idle_tick = apply_idle_tick(ev_ts, cex_price);
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
                }
            }
        }

        sample_net_apy(ev_ts);
        ev_idx = next_event_index(ev_idx + 1);
    }
    result.apy_net_gm = apy_net_gm.value();
    result.apy_net_robust_90d = apy_net_robust_90d.value();
    // Fast skip may omit the last no-trade event; mark at its price anyway.
    const T final_market_price = static_cast<T>(events.p_cex[final_event_idx]);
    result.pool_nav_vs_hold = pool_nav_vs_hold(initial_balances, pool.balances, final_market_price);
    if constexpr (EnableYb) {
        // Raw YB APY is an endpoint metric. If the last event was between
        // hourly reports, mark it once without adding another GM window.
        if constexpr (std::is_floating_point_v<T>) {
            const auto sample_yb_endpoint = [&](const auto& market) {
                yb->external_equity.sample(pool, market, result.t_end, final_market_price);
                if (yb->last_valuation_ts == result.t_end) return;
                yb->apy_tracker.sample(pool, market, result.t_end);
                yb->last_valuation_ts = result.t_end;
            };
            if (yb_2l_on) {
                sample_yb_endpoint(yb->actor);
            }
        }

        result.yb_external_equity_eth = yb_2l_on ? yb->external_equity.equity : -1;
        result.yb_external_growth_eth = yb_2l_on ? yb->external_equity.growth : -1;
        result.yb_external_max_drawdown_hourly = yb_2l_on ? yb->external_equity.max_drawdown : -1;
        result.yb_exposure_return = yb_2l_on ? yb->external_equity.exposure_return : -1;
        result.yb_exposure_rms = yb_2l_on ? yb->external_equity.exposure_rms() : -1;
        result.yb_releverage_fee = yb_2l_on ? yb->actor.state().fee : T(0);
        result.yb_releverage_apy =
            yb_2l_on ? yb->apy_tracker.apy() : -1.0;
        result.yb_releverage_apy_gm = yb->apy_gm.value();
        result.yb_gm30 = yb->apy_gm30.value();
        result.yb_gm60 = yb->apy_gm60.value();
        result.yb_gm30_floor_share = yb->apy_gm30.floor_share();
        result.yb_gm30_unfloored = yb->apy_gm30.unfloored_value();
        result.yb_gm30_windows = static_cast<uint64_t>(yb->apy_gm30.n_windows);
        result.yb_gm60_floor_share = yb->apy_gm60.floor_share();
        result.yb_price_scale_hourly_qv = yb->price_scale_variation.annualized();
        result.yb_releverage_final_growth = yb_2l_on
            ? yb->apy_tracker.final_growth() : -1.0;
        result.yb_releverage_trades = yb_2l_on ? m.yb_2l_fires : 0;
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
    if (!std::isfinite(cfg.yb_min_net_profit_coin0) || cfg.yb_min_net_profit_coin0 < T(0))
        throw std::invalid_argument("yb_min_net_profit_coin0 must be finite and nonnegative");
    if (cfg.yb_arb != YbArb::Levamm && cfg.yb_mode != YbMode::Active2l)
        throw std::invalid_argument("yb_arb='none' requires yb_mode='active_2l'");
    if (cfg.yb_mode == YbMode::Off) {
        return run_event_loop_impl<false>(
            pool, events, costs, dcfg, icfg, ucfg, cfg, candles,
            event_start_floor_ts, out_actions, out_detailed_entries
        );
    }
    return run_event_loop_impl<true>(
        pool, events, costs, dcfg, icfg, ucfg, cfg, candles,
        event_start_floor_ts, out_actions, out_detailed_entries
    );
}

} // namespace harness
} // namespace arb
