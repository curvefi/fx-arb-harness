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
    T arb_guarded_loss_coin0{0};

    // Donations
    size_t donations{0};
    T donation_coin0_total{0};
    std::array<T, 2> donation_amounts_total{T(0), T(0)};

    size_t yb_2l_fires{0};
};

struct TimeWeightedSummary {
    double avg_rel_price_diff{-1.0};
    double max_rel_price_diff{-1.0};
    double max_7d_rel_price_diff{-1.0};
    double final_rel_price_diff{-1.0};
    double detach_energy_ungated{-1.0};
    double avg_imbalance{-1.0};
    double tw_avg_pool_fee{-1.0};
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
        bool floored = true;
        if (std::isfinite(window_growth) && window_growth > F(0)) {
            annualized_apy =
                std::pow(window_growth, SEC_PER_YEAR / dt) - F(1);
            if (!std::isfinite(annualized_apy) || annualized_apy < FLOOR_APY) {
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

};

template <typename F>
struct RollingGeoApy90d : RollingGeoApyWindow<F> {
    static constexpr uint64_t WINDOW_S = 90ULL * 24ULL * 60ULL * 60ULL;
    static constexpr F FLOOR_APY =
        std::is_same_v<F, long double> ? F(1e-20L) : F(1e-20);

    RollingGeoApy90d() : RollingGeoApyWindow<F>(WINDOW_S, FLOOR_APY) {}
};

// Daily 90-day robust net-return score for exhaustive discovery grids. Each
// window contributes its annualized continuously compounded return. The final
// rate gives equal weight to the mean window and the mean of the worst 5% of
// windows. Negative regimes remain finite and rankable; unlike the legacy GM
// of annualized APYs, no arbitrary positive floor is required.
template <typename F>
struct NetApyRobust90d {
    static constexpr uint64_t SAMPLE_S = 24ULL * 60ULL * 60ULL;
    static constexpr uint64_t WINDOW_S = 90ULL * SAMPLE_S;
    static constexpr F SEC_PER_YEAR = F(365.0 * 24.0 * 60.0 * 60.0);
    static constexpr size_t SAMPLE_CAPACITY =
        static_cast<size_t>(WINDOW_S / SAMPLE_S) + 2;
    static constexpr size_t TAIL_DENOMINATOR = 20;

    struct Sample {
        uint64_t ts{0};
        F net_vp{F(0)};
    };

    std::array<Sample, SAMPLE_CAPACITY> samples{};
    size_t head{0};
    size_t count{0};
    uint64_t last_sample_ts{0};
    bool have_sample{false};
    F sum_log_rate{F(0)};
    std::vector<F> log_rates;

    void reserve_duration(uint64_t duration_s) {
        if (duration_s < WINDOW_S) {
            return;
        }
        log_rates.reserve(
            static_cast<size_t>((duration_s - WINDOW_S) / SAMPLE_S) + 1
        );
    }

    bool should_sample(uint64_t ts) const {
        return !have_sample || ts >= last_sample_ts + SAMPLE_S;
    }

    const Sample& sample_at(size_t offset) const {
        return samples[(head + offset) % SAMPLE_CAPACITY];
    }

    void pop_front() {
        head = (head + 1) % SAMPLE_CAPACITY;
        --count;
    }

    void sample(uint64_t ts, F net_vp) {
        if (!std::isfinite(net_vp) || !(net_vp > F(0)) || !should_sample(ts)) {
            return;
        }

        if (count == SAMPLE_CAPACITY) {
            pop_front();
        }
        samples[(head + count) % SAMPLE_CAPACITY] = Sample{ts, net_vp};
        ++count;
        last_sample_ts = ts;
        have_sample = true;

        const uint64_t cutoff = ts > WINDOW_S ? ts - WINDOW_S : 0;
        while (count > 1 && sample_at(1).ts <= cutoff) {
            pop_front();
        }
        const Sample& baseline = sample_at(0);
        if (ts < baseline.ts + WINDOW_S || !(baseline.net_vp > F(0))) {
            return;
        }

        const F dt = static_cast<F>(ts - baseline.ts);
        const F growth = net_vp / baseline.net_vp;
        if (!(dt > F(0)) || !std::isfinite(growth) || !(growth > F(0))) {
            return;
        }
        const F log_rate = std::log(growth) * SEC_PER_YEAR / dt;
        if (!std::isfinite(log_rate)) {
            return;
        }

        sum_log_rate += log_rate;
        log_rates.push_back(log_rate);
    }

    double value() {
        if (log_rates.empty()) {
            return -1.0;
        }
        const size_t tail_count =
            (log_rates.size() + TAIL_DENOMINATOR - 1) / TAIL_DENOMINATOR;
        if (tail_count < log_rates.size()) {
            std::nth_element(
                log_rates.begin(),
                log_rates.begin() + static_cast<std::ptrdiff_t>(tail_count),
                log_rates.end()
            );
        }
        F tail_sum = F(0);
        for (size_t i = 0; i < tail_count; ++i) {
            tail_sum += log_rates[i];
        }
        const F mean_log_rate =
            sum_log_rate / static_cast<F>(log_rates.size());
        const F tail_log_rate = tail_sum / static_cast<F>(tail_count);
        return static_cast<double>(std::expm1(
            (mean_log_rate + tail_log_rate) / F(2)
        ));
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
    std::array<F, PRICE_DIFF_BUCKETS> rel_bucket_sum_dt{};
    std::array<F, PRICE_DIFF_BUCKETS> rel_bucket_dt{};
    std::array<uint64_t, PRICE_DIFF_BUCKETS> rel_bucket_id{};
    std::array<bool, PRICE_DIFF_BUCKETS> rel_bucket_live{};
    F rel_window_sum_dt{F(0)};
    F rel_window_dt{F(0)};
    uint64_t rel_oldest_bucket{0};
    bool have_rel_oldest_bucket{false};
    uint64_t last_7d_eval_bucket{0};
    F max_7d_rel_abs{F(0)};
    bool have_7d_rel_abs{false};

    T last_rel_gap{0};

    static constexpr double DETACH_TH_IN = 0.015;
    F detach_ungated_total{F(0)};

    F sum_imbalance_dt{F(0)};
    F imbalance_dt{F(0)};
    T last_imbalance{0};
    uint64_t last_ts_imbalance{0};
    bool have_imbalance{false};

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
        summary.detach_energy_ungated =
            have_err ? static_cast<double>(detach_ungated_total) : -1.0;
        summary.avg_imbalance = imbalance_dt > F(0)
            ? static_cast<double>(sum_imbalance_dt / imbalance_dt)
            : -1.0;
        summary.tw_avg_pool_fee = tw_fee_dt > F(0)
            ? static_cast<double>(tw_fee_sum_dt / tw_fee_dt)
            : -1.0;
        summary.min_pool_fee = have_fee ? static_cast<double>(min_fee_frac) : -1.0;
        summary.max_pool_fee = have_fee ? static_cast<double>(max_fee_frac) : -1.0;
        return summary;
    }

    template <bool GridCore = false>
    void sample_price_error(uint64_t ts, T price_scale, T p_cex) {
        if (!have_err) {
            first_ts_err = ts;
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
            const F th_in = static_cast<F>(DETACH_TH_IN);
            if (g > th_in) {
                const F ex = g - th_in;
                detach_ungated_total += ex * ex * dt / F(86400);
            }
            if constexpr (GridCore) {
                sample_7d_price_error_incremental(
                    last_ts_err, ts, static_cast<F>(last_rel_abs)
                );
            } else {
                sample_7d_price_error(
                    last_ts_err, ts, static_cast<F>(last_rel_abs)
                );
            }
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

    void sample_7d_price_error_incremental(
        uint64_t start_ts,
        uint64_t end_ts,
        F rel_abs
    ) {
        while (start_ts < end_ts) {
            const uint64_t bucket_id = start_ts / PRICE_DIFF_BUCKET_S;
            const uint64_t bucket_end = (bucket_id + 1) * PRICE_DIFF_BUCKET_S;
            const uint64_t segment_end = end_ts < bucket_end ? end_ts : bucket_end;
            const size_t idx = static_cast<size_t>(bucket_id % PRICE_DIFF_BUCKETS);

            if (!rel_bucket_live[idx] || rel_bucket_id[idx] != bucket_id) {
                if (rel_bucket_live[idx]) {
                    rel_window_sum_dt -= rel_bucket_sum_dt[idx];
                    rel_window_dt -= rel_bucket_dt[idx];
                }
                rel_bucket_live[idx] = true;
                rel_bucket_id[idx] = bucket_id;
                rel_bucket_sum_dt[idx] = F(0);
                rel_bucket_dt[idx] = F(0);
            }
            if (!have_rel_oldest_bucket) {
                rel_oldest_bucket = bucket_id;
                have_rel_oldest_bucket = true;
            }

            const F dt = static_cast<F>(segment_end - start_ts);
            const F weighted = rel_abs * dt;
            rel_bucket_sum_dt[idx] += weighted;
            rel_bucket_dt[idx] += dt;
            rel_window_sum_dt += weighted;
            rel_window_dt += dt;
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

        const uint64_t cutoff = end_ts - PRICE_DIFF_WINDOW_S;
        const uint64_t keep_from = cutoff / PRICE_DIFF_BUCKET_S;
        while (have_rel_oldest_bucket && rel_oldest_bucket < keep_from) {
            const size_t idx = static_cast<size_t>(
                rel_oldest_bucket % PRICE_DIFF_BUCKETS
            );
            if (
                rel_bucket_live[idx] &&
                rel_bucket_id[idx] == rel_oldest_bucket
            ) {
                rel_window_sum_dt -= rel_bucket_sum_dt[idx];
                rel_window_dt -= rel_bucket_dt[idx];
                rel_bucket_live[idx] = false;
            }
            ++rel_oldest_bucket;
        }

        if (rel_window_dt > F(0)) {
            const F avg = rel_window_sum_dt / rel_window_dt;
            if (!have_7d_rel_abs || avg > max_7d_rel_abs) {
                max_7d_rel_abs = avg;
                have_7d_rel_abs = true;
            }
        }
    }

    void sample_imbalance(uint64_t ts, T x0p, T x1p) {
        T current = T(0);
        const T denominator = x0p + x1p;
        if (denominator > T(0)) {
            current = (T(4) * x0p * x1p) / (denominator * denominator);
        }
        if (have_imbalance && ts > last_ts_imbalance) {
            const F dt = static_cast<F>(ts - last_ts_imbalance);
            sum_imbalance_dt += static_cast<F>(last_imbalance) * dt;
            imbalance_dt += dt;
        }
        last_imbalance = current;
        last_ts_imbalance = ts;
        have_imbalance = true;
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
    double apy_net_robust_90d{-1.0};

    // YieldBasis metric family. Filled by the state-mutating active_2l or
    // reference_2l actor.
    T yb_releverage_fee{T(0)};
    double yb_releverage_apy{-1.0};
    double yb_releverage_apy_gm{-1.0};
    double yb_releverage_final_growth{-1.0};
    uint64_t yb_releverage_trades{0};
    uint64_t yb_releverage_gm_windows{0};
    uint64_t yb_releverage_gm_floored_windows{0};
    double yb_releverage_gm_floor_share{-1.0};
};

} // namespace harness
} // namespace arb
