// Metrics tracking for arbitrage harness.
//
// Accumulator precision follows MetricF<T> (harness/precision.hpp): double
// for float/double pool paths (the M2-validated bit-identical reference,
// cross-platform reproducible), long double for the long double build (the
// cluster ld path keeps platform-native extended precision end-to-end).
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

#include "harness/actions.hpp"
#include "harness/detailed_output.hpp"
#include "harness/precision.hpp"

namespace arb {
namespace harness {

// Compounded donation growth over `elapsed_s` at `apy` (per year), compounded
// every `freq_s` seconds (continuous when freq_s == 0). Shared by the in-loop
// apy_net_gm accumulator (MetricF precision) and the end-of-run apy_net path
// (double) so the two stay semantically locked together.
template <typename F>
inline F donation_growth(F apy, F freq_s, F elapsed_s) {
    constexpr F SEC_PER_YEAR = F(365.0 * 24.0 * 60.0 * 60.0);
    if (!(apy > F(0)) || !(elapsed_s > F(0))) {
        return F(1);
    }
    if (freq_s > F(0)) {
        const F period_rate = apy * freq_s / SEC_PER_YEAR;
        if (period_rate <= F(-1)) {
            return F(-1);
        }
        return std::pow(F(1) + period_rate, elapsed_s / freq_s);
    }
    return std::pow(F(1) + apy, elapsed_s / SEC_PER_YEAR);
}

// Core trading metrics
template <typename T>
struct Metrics {
    // Trade execution
    size_t trades{0};
    T notional{0};              // Total notional in coin0 units
    T lp_fee_coin0{0};          // Total LP fees in coin0 units
    T arb_pnl_coin0{0};         // Arbitrageur profit in coin0 units
    size_t n_rebalances{0};     // Count of price_scale changes
    size_t fixed_keeper_ticks{0};  // Executed fixed-cadence tick transactions
    size_t keeper_successful_submissions{0};
    size_t arb_edge_candidates{0};
    size_t arb_invalid_size_rejections{0};
    size_t arb_nonpositive_profit_rejections{0};
    T arb_guarded_loss_coin0{0};

    // Edge-gate telemetry for validating geometry and fee-cache efficiency.
    size_t events_total{0};
    size_t geometry_refreshes{0};
    size_t floor_gate_passes{0};
    size_t actual_fee_calls{0};

    // Commit-gated dynamic keeper.  Attempts are speculative pool ticks;
    // commits are the subset which changed price_scale exactly.
    size_t dynamic_keeper_attempts{0};
    size_t dynamic_keeper_commits{0};
    size_t dynamic_keeper_gap_checks{0};
    size_t dynamic_keeper_gap_fires{0};
    size_t dynamic_keeper_gap_threshold_fires{0};
    size_t dynamic_keeper_heartbeat_fires{0};
    size_t dynamic_keeper_commit_clock_fires{0};
    double dynamic_keeper_step_bps_sum{0};
    double dynamic_keeper_step_bps_max{0};

    // Policy-owned keeper anatomy.  These counters are harness-owned so a
    // rejected speculative transaction can roll the pool back byte-for-byte
    // without erasing the reason it was rejected.
    size_t policy_keeper_checks{0};
    size_t policy_keeper_reject_clock{0};
    size_t policy_keeper_reject_target_unavailable{0};
    size_t policy_keeper_reject_deadband{0};
    size_t policy_keeper_reject_step_min{0};
    size_t policy_keeper_reject_below_threshold{0};
    size_t policy_keeper_reject_block{0};
    size_t policy_keeper_reject_outer_profit{0};
    size_t policy_keeper_raw_gap_candidates{0};
    size_t policy_keeper_submissions{0};
    size_t policy_keeper_submitted_commits{0};
    size_t policy_keeper_final_lp_rejects{0};
    size_t policy_keeper_unexpected_step_rejects{0};
    size_t policy_keeper_exceptions{0};
    size_t policy_keeper_lp_below_precision{0};
    size_t policy_keeper_lp_below_floor{0};
    size_t policy_keeper_lp_burn_cap_exhausted{0};
    size_t policy_keeper_direction_up{0};
    size_t policy_keeper_direction_down{0};

    // Donations
    size_t donations{0};
    T donation_coin0_total{0};
    std::array<T, 2> donation_amounts_total{T(0), T(0)};

    size_t yb_2l_attempts{0};
    size_t yb_2l_fires{0};
    std::array<size_t, 2> yb_2l_fires_by_direction{};
    std::array<T, 2> yb_2l_input_by_direction{};
    std::array<T, 2> yb_2l_output_by_direction{};
    std::array<T, 2> yb_2l_profit_by_direction{};
    double yb_2l_target_log_error_sum{0.0};
    double yb_2l_target_log_error_max{0.0};
    size_t yb_2l_abstain_no_band{0};
    size_t yb_2l_abstain_min_fill{0};
    size_t yb_2l_abstain_invalid_state{0};
    size_t yb_2l_abstain_negative_discriminant{0};
    size_t yb_2l_abstain_debt_floor{0};
    size_t yb_2l_abstain_stable_cash{0};
    size_t yb_2l_abstain_unsafe_debt{0};
    size_t yb_2l_abstain_bad_final_state{0};
    size_t yb_2l_donations{0};
    size_t yb_2l_donation_ps_moves{0};
    size_t yb_2l_fires_without_donation{0};
    size_t yb_2l_donation_reject_cap{0};
    size_t yb_2l_donation_reject_min_mint{0};
    size_t yb_2l_donation_reject_nothing_minted{0};
    size_t yb_2l_donation_reject_tweak_throw{0};
    T yb_2l_donation_coin0{};
    uint64_t yb_2l_first_fire_ts{0};
    uint64_t yb_2l_last_fire_ts{0};
    T yb_2l_start_collateral{};
    T yb_2l_start_debt{};
    T yb_2l_start_stable_balance{};
    T yb_2l_end_collateral{};
    T yb_2l_end_debt_projected{};
    T yb_2l_end_stable_balance{};
    T yb_2l_end_pending_interest{};
    T yb_2l_end_pending_donation{};
    T yb_2l_interest_accrued{};
    T yb_2l_interest_donated{};
    T yb_2l_interest_conservation_residual{};
    T yb_2l_conservation_gap_end{};
    T yb_2l_conservation_gap_max{};
    size_t yb_2l_conservation_checks{0};
    size_t yb_2l_conservation_violations{0};
    size_t yb_2l_conservation_abstains{0};
    size_t yb_2l_fill_adds{0};
    size_t yb_2l_fill_removes{0};
    size_t yb_2l_fill_add_ps_moves{0};
    size_t yb_2l_fill_leg_aborts{0};
    size_t yb_2l_postadd_aborts{0};
    double yb_2l_full_vp_log_growth{0.0};
    double yb_2l_xcp_log_growth{0.0};
    double yb_2l_burn_backfill_log_growth{0.0};
};

struct TimeWeightedSummary {
    double avg_rel_price_diff{-1.0};
    double max_rel_price_diff{-1.0};
    double max_7d_rel_price_diff{-1.0};
    double final_rel_price_diff{-1.0};
    double max_episode_gap_energy{-1.0};
    double detach_energy{-1.0};
    double detach_energy_ungated{-1.0};
    double detach_energy_ungated_3pct{-1.0};
    double detach_energy_ungated_5pct{-1.0};
    double detach_energy_short3h{-1.0};
    double avg_imbalance{-1.0};
    double max_7d_skew{-1.0};
    double tw_avg_pool_fee{-1.0};
    double min_price_scale{-1.0};
    double max_price_scale{-1.0};
    double min_pool_fee{-1.0};
    double max_pool_fee{-1.0};
};

template <typename F>
struct RollingGeoApyWindow {
    static constexpr uint64_t SAMPLE_S = 60ULL * 60ULL;
    static constexpr F SEC_PER_YEAR = F(365.0 * 24.0 * 60.0 * 60.0);

    uint64_t WINDOW_S;
    F FLOOR_APY;

    struct Sample {
        uint64_t ts{0};
        F net_vp{F(0)};
    };

    std::deque<Sample> samples;
    uint64_t last_sample_ts{0};
    bool have_sample{false};
    F sum_log_apy{F(0)};
    size_t n_windows{0};
    size_t n_floored_windows{0};
    std::vector<F> window_apys;

    RollingGeoApyWindow(uint64_t window_s, F floor_apy)
        : WINDOW_S(window_s), FLOOR_APY(floor_apy) {}

    bool should_sample(uint64_t ts) const {
        return !have_sample || ts >= last_sample_ts + SAMPLE_S;
    }

    void sample(uint64_t ts, F net_vp) {
        if (!std::isfinite(net_vp) || !(net_vp > F(0))) {
            return;
        }
        if (!should_sample(ts)) {
            return;
        }

        samples.push_back(Sample{ts, net_vp});
        last_sample_ts = ts;
        have_sample = true;

        const uint64_t cutoff = ts > WINDOW_S ? ts - WINDOW_S : 0;
        while (samples.size() > 1 && samples[1].ts <= cutoff) {
            samples.pop_front();
        }

        if (samples.empty() || ts < samples.front().ts + WINDOW_S) {
            return;
        }

        const F dt = static_cast<F>(ts - samples.front().ts);
        if (!(dt > F(0)) || !(samples.front().net_vp > F(0))) {
            return;
        }

        const F window_growth = net_vp / samples.front().net_vp;
        F annualized_apy = FLOOR_APY;
        F raw_apy = FLOOR_APY;
        bool have_raw = false;
        bool floored = true;
        if (std::isfinite(window_growth) && window_growth > F(0)) {
            raw_apy = std::pow(window_growth, SEC_PER_YEAR / dt) - F(1);
            have_raw = std::isfinite(raw_apy);
            annualized_apy = raw_apy;
            if (!have_raw || annualized_apy < FLOOR_APY) {
                annualized_apy = FLOOR_APY;
            } else {
                floored = false;
            }
        }

        sum_log_apy += std::log(annualized_apy);
        n_windows += 1;
        if (floored) {
            n_floored_windows += 1;
        }
        window_apys.push_back(have_raw ? raw_apy : FLOOR_APY);
    }

    double value() const {
        if (n_windows == 0) {
            return -1.0;
        }
        return static_cast<double>(std::exp(sum_log_apy / static_cast<F>(n_windows)));
    }

    double floor_share() const {
        if (n_windows == 0) {
            return -1.0;
        }
        return static_cast<double>(n_floored_windows) /
            static_cast<double>(n_windows);
    }

    double worst_window() const {
        if (window_apys.empty()) {
            return -1.0;
        }
        return static_cast<double>(*std::min_element(window_apys.begin(), window_apys.end()));
    }

    double median_window() const {
        if (window_apys.empty()) {
            return -1.0;
        }
        std::vector<F> tmp(window_apys.begin(), window_apys.end());
        const size_t n = tmp.size();
        const size_t mid = n / 2;
        std::nth_element(tmp.begin(), tmp.begin() + mid, tmp.end());
        const F hi = tmp[mid];
        if (n % 2 != 0) {
            return static_cast<double>(hi);
        }
        const F lo = *std::max_element(tmp.begin(), tmp.begin() + mid);
        return static_cast<double>((lo + hi) / F(2));
    }

    double cvar_low_window(double fraction) const {
        if (window_apys.empty() || !(fraction > 0.0)) {
            return -1.0;
        }
        std::vector<F> tmp(window_apys.begin(), window_apys.end());
        std::sort(tmp.begin(), tmp.end());
        size_t n = static_cast<size_t>(
            std::ceil(static_cast<double>(tmp.size()) * fraction)
        );
        n = std::clamp<size_t>(n, 1, tmp.size());
        F sum = F(0);
        for (size_t i = 0; i < n; ++i) {
            sum += tmp[i];
        }
        return static_cast<double>(sum / static_cast<F>(n));
    }
};

template <typename F>
struct RollingGeoApy90d : RollingGeoApyWindow<F> {
    static constexpr uint64_t WINDOW_S = 90ULL * 24ULL * 60ULL * 60ULL;
    static constexpr F FLOOR_APY =
        std::is_same_v<F, long double> ? F(1e-20L) : F(1e-20);

    RollingGeoApy90d() : RollingGeoApyWindow<F>(WINDOW_S, FLOOR_APY) {}
};

template <typename F>
struct MultiScalePositiveGrowthConcentration {
    static constexpr uint64_t SAMPLE_S = 24ULL * 60ULL * 60ULL;

    uint64_t last_sample_ts{0};
    bool have_sample{false};
    F last_value{F(0)};
    F positive_sum{F(0)};
    std::vector<F> daily_positive_increments;

    bool should_sample(uint64_t ts) const {
        return !have_sample || ts >= last_sample_ts + SAMPLE_S;
    }

    void sample(uint64_t ts, F value) {
        if (!std::isfinite(value) || !(value > F(0))) {
            return;
        }
        if (!should_sample(ts)) {
            return;
        }
        if (have_sample) {
            const F delta = value - last_value;
            const F positive_delta =
                (std::isfinite(delta) && delta > F(0)) ? delta : F(0);
            daily_positive_increments.push_back(positive_delta);
            positive_sum += positive_delta;
        }
        last_sample_ts = ts;
        last_value = value;
        have_sample = true;
    }

    double block_share(size_t block_days, size_t k) const {
        const F eps = std::is_same_v<F, long double> ? F(1e-30L) : F(1e-30);
        if (
            !(positive_sum > eps) ||
            daily_positive_increments.empty() ||
            block_days == 0 ||
            k == 0
        ) {
            return 0.0;
        }
        std::vector<F> blocks;
        blocks.reserve((daily_positive_increments.size() + block_days - 1) / block_days);
        for (size_t i = 0; i < daily_positive_increments.size(); i += block_days) {
            F block_sum = F(0);
            const size_t end = std::min(i + block_days, daily_positive_increments.size());
            for (size_t j = i; j < end; ++j) {
                block_sum += daily_positive_increments[j];
            }
            blocks.push_back(block_sum);
        }
        std::sort(blocks.begin(), blocks.end(), std::greater<F>());
        const size_t n = std::min(k, blocks.size());
        F top_sum = F(0);
        for (size_t i = 0; i < n; ++i) {
            top_sum += blocks[i];
        }
        return static_cast<double>(top_sum / positive_sum);
    }
};

template <typename T>
struct TimeWeightedMetrics {
    using F = MetricF<T>;
    static constexpr uint64_t PRICE_DIFF_WINDOW_S = 7 * 24 * 60 * 60;
    static constexpr uint64_t PRICE_DIFF_BUCKET_S = 60 * 60;
    static constexpr size_t PRICE_DIFF_BUCKETS =
        PRICE_DIFF_WINDOW_S / PRICE_DIFF_BUCKET_S + 2;

    F sum_abs_rel_dt{F(0)};
    F sum_dt{F(0)};
    F max_rel_abs{F(0)};
    T last_rel_abs{0};
    uint64_t first_ts_err{0};
    uint64_t last_ts_err{0};
    bool have_err{false};
    T min_price_scale{0};
    T max_price_scale{0};
    bool have_price_scale{false};
    std::array<F, PRICE_DIFF_BUCKETS> rel_bucket_sum_dt{};
    std::array<F, PRICE_DIFF_BUCKETS> rel_bucket_dt{};
    std::array<uint64_t, PRICE_DIFF_BUCKETS> rel_bucket_id{};
    std::array<bool, PRICE_DIFF_BUCKETS> rel_bucket_live{};
    uint64_t last_7d_eval_bucket{0};
    F max_7d_rel_abs{F(0)};
    bool have_7d_rel_abs{false};

    T last_rel_gap{0};

    static constexpr double EPISODE_GAP_THRESHOLD = 0.03;
    F max_episode_gap_energy_acc{F(0)};
    F cur_episode_gap_energy{F(0)};

    static constexpr double DETACH_TH_IN = 0.015;
    static constexpr double DETACH_TH_OUT = 0.010;
    static constexpr uint64_t DETACH_MIN_DURATION_S = 24ULL * 60ULL * 60ULL;
    bool detach_open{false};
    uint64_t detach_ep_start_ts{0};
    F detach_ep_energy{F(0)};
    F detach_total{F(0)};

    static constexpr uint64_t DETACH_SHORT_MIN_DURATION_S = 3ULL * 60ULL * 60ULL;
    static constexpr double DETACH_UNGATED_TH_3PCT = 0.030;
    static constexpr double DETACH_UNGATED_TH_5PCT = 0.050;
    F detach_ungated_total{F(0)};
    F detach_ungated_3pct_total{F(0)};
    F detach_ungated_5pct_total{F(0)};
    bool detach_short_open{false};
    uint64_t detach_short_ep_start_ts{0};
    F detach_short_ep_energy{F(0)};
    F detach_short_total{F(0)};

    F sum_imbalance_dt{F(0)};
    F imbalance_dt{F(0)};
    T last_imbalance{0};
    T last_skew{0};
    uint64_t first_ts_imbalance{0};
    uint64_t last_ts_imbalance{0};
    bool have_imbalance{false};
    std::array<F, PRICE_DIFF_BUCKETS> skew_bucket_sum_dt{};
    std::array<F, PRICE_DIFF_BUCKETS> skew_bucket_dt{};
    std::array<uint64_t, PRICE_DIFF_BUCKETS> skew_bucket_id{};
    std::array<bool, PRICE_DIFF_BUCKETS> skew_bucket_live{};
    uint64_t last_7d_skew_eval_bucket{0};
    F max_7d_skew{F(0)};
    bool have_7d_skew{false};

    F tw_fee_sum_dt{F(0)};
    F tw_fee_dt{F(0)};
    T last_fee_frac{0};
    uint64_t last_ts_fee{0};
    bool have_fee{false};
    T min_fee_frac{0};
    T max_fee_frac{0};

    TimeWeightedSummary summarize() const {
        TimeWeightedSummary summary{};
        summary.avg_rel_price_diff = sum_dt > F(0)
            ? static_cast<double>(sum_abs_rel_dt / sum_dt)
            : -1.0;
        summary.max_rel_price_diff = static_cast<double>(max_rel_abs);
        summary.max_7d_rel_price_diff = have_7d_rel_abs
            ? static_cast<double>(max_7d_rel_abs)
            : -1.0;
        summary.final_rel_price_diff = have_err
            ? static_cast<double>(last_rel_abs)
            : -1.0;
        summary.max_episode_gap_energy = have_err
            ? static_cast<double>(
                  std::max(max_episode_gap_energy_acc, cur_episode_gap_energy))
            : -1.0;
        {
            F detach_final = detach_total;
            if (detach_open &&
                last_ts_err - detach_ep_start_ts >= DETACH_MIN_DURATION_S) {
                detach_final += detach_ep_energy;
            }
            summary.detach_energy =
                have_err ? static_cast<double>(detach_final) : -1.0;
        }
        summary.detach_energy_ungated =
            have_err ? static_cast<double>(detach_ungated_total) : -1.0;
        summary.detach_energy_ungated_3pct =
            have_err ? static_cast<double>(detach_ungated_3pct_total) : -1.0;
        summary.detach_energy_ungated_5pct =
            have_err ? static_cast<double>(detach_ungated_5pct_total) : -1.0;
        {
            F detach_short_final = detach_short_total;
            if (detach_short_open &&
                last_ts_err - detach_short_ep_start_ts >= DETACH_SHORT_MIN_DURATION_S) {
                detach_short_final += detach_short_ep_energy;
            }
            summary.detach_energy_short3h =
                have_err ? static_cast<double>(detach_short_final) : -1.0;
        }
        summary.avg_imbalance = imbalance_dt > F(0)
            ? static_cast<double>(sum_imbalance_dt / imbalance_dt)
            : -1.0;
        summary.max_7d_skew = have_7d_skew
            ? static_cast<double>(max_7d_skew)
            : -1.0;
        summary.tw_avg_pool_fee = tw_fee_dt > F(0)
            ? static_cast<double>(tw_fee_sum_dt / tw_fee_dt)
            : -1.0;
        summary.min_price_scale = have_price_scale ? static_cast<double>(min_price_scale) : -1.0;
        summary.max_price_scale = have_price_scale ? static_cast<double>(max_price_scale) : -1.0;
        summary.min_pool_fee = have_fee ? static_cast<double>(min_fee_frac) : -1.0;
        summary.max_pool_fee = have_fee ? static_cast<double>(max_fee_frac) : -1.0;
        return summary;
    }

    void sample_price_error(uint64_t ts, T price_scale, T p_cex) {
        if (!have_err) {
            first_ts_err = ts;
        }

        if (!have_price_scale) {
            min_price_scale = price_scale;
            max_price_scale = price_scale;
            have_price_scale = true;
        } else {
            if (price_scale < min_price_scale) min_price_scale = price_scale;
            if (price_scale > max_price_scale) max_price_scale = price_scale;
        }

        T cur_rel_abs = T(0);
        if (p_cex > T(0)) {
            cur_rel_abs = std::abs(price_scale / p_cex - T(1));
        }
        T cur_rel_gap = T(0);
        if (price_scale > T(0) && p_cex > T(0)) {
            cur_rel_gap = std::min(std::abs(p_cex / price_scale - T(1)), T(1));
        }

        if (have_err && ts > last_ts_err) {
            const F dt = static_cast<F>(ts - last_ts_err);
            sum_abs_rel_dt += static_cast<F>(last_rel_abs) * dt;
            const F g = static_cast<F>(last_rel_gap);
            sum_dt += dt;
            const F thr = static_cast<F>(EPISODE_GAP_THRESHOLD);
            if (g > thr) {
                cur_episode_gap_energy += (g - thr) * dt / F(86400);
            } else {
                if (cur_episode_gap_energy > max_episode_gap_energy_acc) {
                    max_episode_gap_energy_acc = cur_episode_gap_energy;
                }
                cur_episode_gap_energy = F(0);
            }
            const F th_in = static_cast<F>(DETACH_TH_IN);
            const F th_out = static_cast<F>(DETACH_TH_OUT);
            if (g > th_in) {
                const F ex = g - th_in;
                detach_ungated_total += ex * ex * dt / F(86400);
            }
            const F th_3pct = static_cast<F>(DETACH_UNGATED_TH_3PCT);
            if (g > th_3pct) {
                const F ex = g - th_3pct;
                detach_ungated_3pct_total += ex * ex * dt / F(86400);
            }
            const F th_5pct = static_cast<F>(DETACH_UNGATED_TH_5PCT);
            if (g > th_5pct) {
                const F ex = g - th_5pct;
                detach_ungated_5pct_total += ex * ex * dt / F(86400);
            }
            if (!detach_open) {
                if (g > th_in) {
                    detach_open = true;
                    detach_ep_start_ts = last_ts_err;
                    const F ex = g - th_in;
                    detach_ep_energy = ex * ex * dt / F(86400);
                    detach_short_open = true;
                    detach_short_ep_start_ts = last_ts_err;
                    detach_short_ep_energy = ex * ex * dt / F(86400);
                }
            } else if (g < th_out) {
                if (last_ts_err - detach_ep_start_ts >= DETACH_MIN_DURATION_S) {
                    detach_total += detach_ep_energy;
                }
                detach_open = false;
                detach_ep_energy = F(0);
                if (detach_short_open &&
                    last_ts_err - detach_short_ep_start_ts >= DETACH_SHORT_MIN_DURATION_S) {
                    detach_short_total += detach_short_ep_energy;
                }
                detach_short_open = false;
                detach_short_ep_energy = F(0);
            } else if (g > th_in) {
                const F ex = g - th_in;
                detach_ep_energy += ex * ex * dt / F(86400);
                if (!detach_short_open) {
                    detach_short_open = true;
                    detach_short_ep_start_ts = last_ts_err;
                    detach_short_ep_energy = ex * ex * dt / F(86400);
                } else {
                    detach_short_ep_energy += ex * ex * dt / F(86400);
                }
            }
            sample_7d_price_error(last_ts_err, ts, static_cast<F>(last_rel_abs));
        }

        if (static_cast<F>(cur_rel_abs) > max_rel_abs) {
            max_rel_abs = static_cast<F>(cur_rel_abs);
        }

        last_rel_abs = cur_rel_abs;
        last_rel_gap = cur_rel_gap;
        last_ts_err = ts;
        have_err = true;
    }

    void sample_7d_price_error(uint64_t start_ts, uint64_t end_ts, F rel_abs) {
        while (start_ts < end_ts) {
            const uint64_t bucket_id = start_ts / PRICE_DIFF_BUCKET_S;
            const uint64_t bucket_end = (bucket_id + 1) * PRICE_DIFF_BUCKET_S;
            const uint64_t segment_end = end_ts < bucket_end ? end_ts : bucket_end;
            const size_t idx = static_cast<size_t>(bucket_id % PRICE_DIFF_BUCKETS);

            if (!rel_bucket_live[idx] || rel_bucket_id[idx] != bucket_id) {
                rel_bucket_live[idx] = true;
                rel_bucket_id[idx] = bucket_id;
                rel_bucket_sum_dt[idx] = F(0);
                rel_bucket_dt[idx] = F(0);
            }

            const F dt = static_cast<F>(segment_end - start_ts);
            rel_bucket_sum_dt[idx] += rel_abs * dt;
            rel_bucket_dt[idx] += dt;
            start_ts = segment_end;
        }

        const uint64_t eval_bucket = end_ts / PRICE_DIFF_BUCKET_S;
        if (have_7d_rel_abs && eval_bucket == last_7d_eval_bucket) {
            return;
        }
        last_7d_eval_bucket = eval_bucket;
        if (end_ts < first_ts_err + PRICE_DIFF_WINDOW_S) {
            return;
        }

        const uint64_t cutoff = end_ts > PRICE_DIFF_WINDOW_S
            ? end_ts - PRICE_DIFF_WINDOW_S
            : 0;
        F window_sum_dt = F(0);
        F window_dt = F(0);
        for (size_t i = 0; i < PRICE_DIFF_BUCKETS; ++i) {
            if (!rel_bucket_live[i]) {
                continue;
            }
            const uint64_t bucket_start = rel_bucket_id[i] * PRICE_DIFF_BUCKET_S;
            const uint64_t bucket_end = bucket_start + PRICE_DIFF_BUCKET_S;
            if (bucket_end <= cutoff || bucket_start >= end_ts) {
                continue;
            }
            window_sum_dt += rel_bucket_sum_dt[i];
            window_dt += rel_bucket_dt[i];
        }

        if (window_dt > F(0)) {
            const F avg = window_sum_dt / window_dt;
            if (!have_7d_rel_abs || avg > max_7d_rel_abs) {
                max_7d_rel_abs = avg;
                have_7d_rel_abs = true;
            }
        }
    }

    void sample_imbalance(uint64_t ts, T x0p, T x1p) {
        if (!have_imbalance) {
            first_ts_imbalance = ts;
        }

        T cur_imbalance = T(0);
        T cur_skew = T(0);
        const T denom = x0p + x1p;
        if (denom > T(0)) {
            cur_imbalance = (T(4) * x0p * x1p) / (denom * denom);
            const T dominant = x0p > x1p ? x0p : x1p;
            cur_skew = dominant / denom;
        }

        if (have_imbalance && ts > last_ts_imbalance) {
            const F dt = static_cast<F>(ts - last_ts_imbalance);
            sum_imbalance_dt += static_cast<F>(last_imbalance) * dt;
            imbalance_dt += dt;
            sample_7d_skew(last_ts_imbalance, ts, static_cast<F>(last_skew));
        }

        last_imbalance = cur_imbalance;
        last_skew = cur_skew;
        last_ts_imbalance = ts;
        have_imbalance = true;
    }

    void sample_7d_skew(uint64_t start_ts, uint64_t end_ts, F skew) {
        while (start_ts < end_ts) {
            const uint64_t bucket_id = start_ts / PRICE_DIFF_BUCKET_S;
            const uint64_t bucket_end = (bucket_id + 1) * PRICE_DIFF_BUCKET_S;
            const uint64_t segment_end = end_ts < bucket_end ? end_ts : bucket_end;
            const size_t idx = static_cast<size_t>(bucket_id % PRICE_DIFF_BUCKETS);

            if (!skew_bucket_live[idx] || skew_bucket_id[idx] != bucket_id) {
                skew_bucket_live[idx] = true;
                skew_bucket_id[idx] = bucket_id;
                skew_bucket_sum_dt[idx] = F(0);
                skew_bucket_dt[idx] = F(0);
            }

            const F dt = static_cast<F>(segment_end - start_ts);
            skew_bucket_sum_dt[idx] += skew * dt;
            skew_bucket_dt[idx] += dt;
            start_ts = segment_end;
        }

        const uint64_t eval_bucket = end_ts / PRICE_DIFF_BUCKET_S;
        if (have_7d_skew && eval_bucket == last_7d_skew_eval_bucket) {
            return;
        }
        last_7d_skew_eval_bucket = eval_bucket;
        if (end_ts < first_ts_imbalance + PRICE_DIFF_WINDOW_S) {
            return;
        }

        const uint64_t cutoff = end_ts > PRICE_DIFF_WINDOW_S
            ? end_ts - PRICE_DIFF_WINDOW_S
            : 0;
        F window_sum_dt = F(0);
        F window_dt = F(0);
        for (size_t i = 0; i < PRICE_DIFF_BUCKETS; ++i) {
            if (!skew_bucket_live[i]) {
                continue;
            }
            const uint64_t bucket_start = skew_bucket_id[i] * PRICE_DIFF_BUCKET_S;
            const uint64_t bucket_end = bucket_start + PRICE_DIFF_BUCKET_S;
            if (bucket_end <= cutoff || bucket_start >= end_ts) {
                continue;
            }
            window_sum_dt += skew_bucket_sum_dt[i];
            window_dt += skew_bucket_dt[i];
        }

        if (window_dt > F(0)) {
            const F avg = window_sum_dt / window_dt;
            if (!have_7d_skew || avg > max_7d_skew) {
                max_7d_skew = avg;
                have_7d_skew = true;
            }
        }
    }

    void sample_fee(uint64_t ts, T fee_frac) {
        if (!have_fee) {
            min_fee_frac = fee_frac;
            max_fee_frac = fee_frac;
        } else {
            if (fee_frac < min_fee_frac) min_fee_frac = fee_frac;
            if (fee_frac > max_fee_frac) max_fee_frac = fee_frac;
        }
        if (have_fee && ts > last_ts_fee) {
            const F dt = static_cast<F>(ts - last_ts_fee);
            tw_fee_sum_dt += static_cast<F>(last_fee_frac) * dt;
            tw_fee_dt += dt;
        }
        last_fee_frac = fee_frac;
        last_ts_fee = ts;
        have_fee = true;
    }
};

inline double tvl_growth(double tvl_start, double tvl_end) {
    return tvl_start > 0.0 ? tvl_end / tvl_start : -1.0;
}

template <typename T>
struct SlippageProbes {
    using F = MetricF<T>;
    static constexpr size_t N_SIZES = 3;
    static constexpr double SIZE_FRACS[N_SIZES] = {
        0.01,
        0.05,
        0.10,
    };

    std::array<F, N_SIZES> tw_real_s01_sum_dt{};
    std::array<F, N_SIZES> tw_real_s10_sum_dt{};
    std::array<F, N_SIZES> tw_real_dt{};
    std::array<T, N_SIZES> last_real_s01{};
    std::array<T, N_SIZES> last_real_s10{};
    std::array<uint64_t, N_SIZES> last_ts_real{};
    std::array<bool, N_SIZES> have_real{};

    void accumulate_previous(size_t k, uint64_t ts) {
        if (k >= N_SIZES) return;
        if (have_real[k] && ts > last_ts_real[k]) {
            const F dt = static_cast<F>(ts - last_ts_real[k]);
            tw_real_s01_sum_dt[k] += static_cast<F>(last_real_s01[k]) * dt;
            tw_real_s10_sum_dt[k] += static_cast<F>(last_real_s10[k]) * dt;
            tw_real_dt[k] += dt;
        }
    }

    void sample(size_t k, uint64_t ts, T s01, T s10) {
        if (k >= N_SIZES) return;
        last_real_s01[k] = s01;
        last_real_s10[k] = s10;
        last_ts_real[k] = ts;
        have_real[k] = true;
    }

    double tw_slippage(size_t k) const {
        if (k >= N_SIZES || tw_real_dt[k] <= F(0)) return -1.0;
        return static_cast<double>((tw_real_s01_sum_dt[k] + tw_real_s10_sum_dt[k]) / (F(2) * tw_real_dt[k]));
    }

    double tw_slippage_0to1(size_t k) const {
        if (k >= N_SIZES || tw_real_dt[k] <= F(0)) return -1.0;
        return static_cast<double>(tw_real_s01_sum_dt[k] / tw_real_dt[k]);
    }

    double tw_slippage_1to0(size_t k) const {
        if (k >= N_SIZES || tw_real_dt[k] <= F(0)) return -1.0;
        return static_cast<double>(tw_real_s10_sum_dt[k] / tw_real_dt[k]);
    }
};

template <typename T>
struct EventLoopResult {
    Metrics<T> metrics{};
    TimeWeightedMetrics<T> tw_metrics{};
    SlippageProbes<T> slippage_probes{};

    uint64_t t_start{0};
    uint64_t t_end{0};
    T tvl_start{0};
    T donation_apy{0};
    double apy_net_gm{-1.0};

    // YieldBasis metric family. Filled by the state-mutating active_2l or
    // reference_2l actor.
    bool yb_releverage_enabled{false};
    T yb_releverage_fee{T(0)};
    double yb_releverage_apy{-1.0};
    double yb_releverage_apy_gm{-1.0};
    double yb_releverage_final_growth{-1.0};
    uint64_t yb_releverage_trades{0};
    uint64_t yb_releverage_gm_windows{0};
    uint64_t yb_releverage_gm_floored_windows{0};
    double yb_releverage_gm_floor_share{-1.0};

    double yb_gm_90d_worst_window{-1.0};
    double yb_gm_90d_median_window{-1.0};
    double yb_gm_90d_floor_share{-1.0};
    double yb_gm_30d_cvar20{-1.0};
    double yb_gm_30d_floor_share{-1.0};
    double yb_growth_step_share_1d{-1.0};
    double yb_growth_step_share_3d{-1.0};
    double yb_growth_step_share_7d{-1.0};
    double yb_growth_top10_step_share{-1.0};

};

} // namespace harness
} // namespace arb
