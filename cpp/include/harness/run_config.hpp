// Per-run configuration shared by the event loop and the pool runner.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "events/cex_depth.hpp"
#include "harness/yb_initial_state.hpp"
#include "harness/state_reconciliation.hpp"

namespace arb {
namespace harness {

// YieldBasis operating mode.
enum class YbMode : uint8_t {
    Off = 0,   // no YieldBasis: the yb metric family stays empty
    Active2l,  // state-mutating 2L contract model (the original yb path)
    Reference2l,   // contract-derived VirtualPool/LevAMM reference candidate
};

enum class EventCursor : uint8_t {
    Scalar = 0,
    ExactSkip,
};

enum class MetricProfile : uint8_t {
    FullSummary = 0,
    GridCore,
};

enum class ActorTimingMode : uint8_t { LegacyEvent = 0, MinuteSequential };

template <typename T>
struct RunConfig {
    T min_swap_frac{T(1e-6)};
    T max_swap_frac{T(1.0)};
    uint64_t start_ts{0};
    uint64_t dustswap_freq_s{3600};
    uint64_t user_swap_freq_s{0};
    T user_swap_size_frac{T(0.01)};
    T user_swap_thresh{T(0.05)};
    const events::CexDepthTape* cex_depth{nullptr};
    const events::ObservedStateTape<T>* observed_state{nullptr};
    StateReconciliationMode state_reconciliation_mode{StateReconciliationMode::Off};
    uint64_t equalization_delay_s{60};
    T reset_threshold_bps{T(100)};
    uint64_t observation_interval_s{60};
    uint64_t cex_depth_max_age_s{30};
    ActorTimingMode actor_timing_mode{ActorTimingMode::LegacyEvent};
    bool save_actions{false};

    // Detailed per-event logging
    bool detailed_log{false};
    size_t detailed_interval{1};  // log every N-th event (1 = all)

    // Optional YieldBasis 2L model. Existing modes retain their behavior;
    // reference_2l adds full represented VirtualPool route arithmetic.
    YbMode yb_mode{YbMode::Off};
    T yb_releverage_fee{T(0.012)};
    T yb_cash_multiplier{T(1)};
    T yb_min_net_profit_coin0{T(1)};
    std::optional<YbInitialState<T>> yb_initial_state;

    // Slippage probe sampling
    bool enable_slippage_probes{false};

    // Scalar remains the reference cursor. ExactSkip is admitted only when
    // skipped events are provably observationally irrelevant.
    EventCursor event_cursor{EventCursor::Scalar};
    MetricProfile metric_profile{MetricProfile::FullSummary};

};

} // namespace harness
} // namespace arb
