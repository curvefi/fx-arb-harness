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
