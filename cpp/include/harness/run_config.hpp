// Per-run configuration shared by the event loop and the pool runner.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "harness/yb_initial_state.hpp"

namespace arb {
namespace harness {

// YieldBasis operating mode.
enum class YbMode : uint8_t {
    Off = 0,   // no YieldBasis: the yb metric family stays empty
    Active2l,  // state-mutating 2L contract model (the original yb path)
};

enum class EventCursor : uint8_t {
    Scalar = 0,
    FastSkip,
};


// Whether the modeled YieldBasis actor trades the LEVAMM (its fee-paying exchange) or not at all (active_2l only).
enum class YbArb : uint8_t { Levamm = 0, None };

template <typename T>
struct RunConfig {
    T min_swap_frac{T(1e-6)};
    T max_swap_frac{T(1.0)};
    uint64_t start_ts{0};
    uint64_t dustswap_freq_s{3600};
    uint64_t user_swap_freq_s{0};
    T user_swap_size_frac{T(0.01)};
    T user_swap_thresh{T(0.05)};
    T arb_report_rate{T(1)};
    T arb_report_max_age_s{T(0)};
    T arb_report_count{T(0)}; // 0 preserves time-window mode; >0 selects last N reports.
    T arb_report_random_count{T(0)}; // 0 disables; 1..6 samples one of the last N reports.
    T arb_report_offset{T(-1)}; // -1 preserves window/count mode; 0 is latest, 1 is previous.
    // Block mode: seconds after the block at which this run's event price is taken (0, 1 or 2); -1 = the session's.
    int arb_settle_offset_s{-1};
    // Research: the due touch (dustswap_freq_s since the last one) carries the latest report. 1: the keeper acts
    // before the arbitrageur; 2: after it, in the place of the report-less touch. 0: off.
    int keeper_report{0};
    // Research: with keeper_report on, the keeper also touches (with the latest report) whenever that report is more than
    // keeper_gap away from the price scale and keeper_fast_s have passed since the last touch. After the arbitrageur.
    double keeper_gap{0.0};
    uint64_t keeper_fast_s{12};
    // Research: YieldBasis's borrow rate, which funds the pool donation, is reset every hour to its initial value times
    // clamp(gap / yb_rate_gap, yb_rate_min, yb_rate_max), gap = |latest report / price scale - 1|. 0: constant rate.
    double yb_rate_gap{0.0};
    double yb_rate_min{0.25};
    double yb_rate_max{4.0};
    // Research (yb_rate_target > 0): a gain k, 1 at the start, is multiplied every hour by exp(yb_rate_eta x
    // clamp(gap / yb_rate_target - 1, -1, 3)) and kept within [1 / yb_rate_k_max, yb_rate_k_max] (lower bound yb_rate_k_min when
    // positive); the rate is the initial value times clamp(k x gap / yb_rate_gap, yb_rate_min, yb_rate_max). 0: k stays 1.
    double yb_rate_target{0.0};
    double yb_rate_eta{0.005};
    double yb_rate_k_max{8.0};
    double yb_rate_k_min{0.0};
    // yb_rate_aw 1: the gain is held while the rate sits at its ceiling or floor. yb_rate_eta_down > 0: the speed of a falling
    // gain, yb_rate_eta that of a rising one.
    int yb_rate_aw{0};
    double yb_rate_eta_down{0.0};
    // yb_rate_fade > 0: beyond this gap the rate is multiplied by yb_rate_fade / gap.
    double yb_rate_fade{0.0};
    // Research: when positive, YieldBasis's interest distribution is also called on its own every this many seconds (at the
    // first event at or after the due time, after the arbitrageur, the fill and the keeper; with keeper_report on it carries
    // the latest report). 0: only as part of a fill.
    uint64_t yb_distribute_s{0};
    // yb_distribute_gap > 0: that separate distribution happens only while the latest report is at least this far from the
    // price scale. 0: at every due time.
    double yb_distribute_gap{0.0};
    // Research: when positive, the LEVAMM reads the LP oracle at an EMA of the price scale (in logs, this half-life in
    // seconds) instead of the scale itself. Scalar event cursor only. 0: off.
    double yb_oracle_half_life_s{0.0};

    // Detailed per-event logging
    size_t detailed_interval{1};  // log every N-th event (1 = all)

    // Optional YieldBasis 2L model.
    YbMode yb_mode{YbMode::Off};
    T yb_releverage_fee{T(0.012)};
    T yb_cash_multiplier{T(1)};
    T yb_min_net_profit_coin0{T(1)};
    YbArb yb_arb{YbArb::Levamm};
    // active_2l: the actor values coin 1 at the event price -/+ this many bp (bid/ask), i.e. it closes short of
    // the LEVAMM fee-band edge by about half that in LP terms.
    T yb_execution_bps{T(5)};
    std::optional<YbInitialState<T>> yb_initial_state;

    // Slippage probe sampling
    bool enable_slippage_probes{false};

    // Scalar remains the reference cursor. FastSkip bypasses only events that
    // are provably observationally irrelevant.
    EventCursor event_cursor{EventCursor::Scalar};

    // Positive: end the run once max_7d_rel_price_diff exceeds this value.
    double early_stop_max_7d_rel_price_diff{0.0};

};

} // namespace harness
} // namespace arb
