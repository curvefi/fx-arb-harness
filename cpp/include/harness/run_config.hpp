// Per-run configuration shared by the event loop and the pool runner.
#pragma once

#include <cstddef>
#include <cstdint>

namespace arb {
namespace harness {

inline constexpr uint64_t DYNAMIC_KEEPER_RETRY_S = 60;
inline constexpr uint64_t POLICY_KEEPER_RETRY_S = 10;

// YieldBasis operating mode.
enum class YbMode : uint8_t {
    Off = 0,   // no YieldBasis: the yb metric family stays empty
    Passive,   // metrics-only projection: a private second simulation runs the
               // exact active_2l transition on a copied initial pool and only
               // the yb metric family is adopted. The primary pool, donation
               // schedule, and actor state are never mutated. Costs roughly a
               // second full simulation, so it is opt-in.
    Active2l,  // state-mutating 2L contract model (the original yb path)
};

template <typename T>
struct RunConfig {
    T min_swap_frac{T(1e-6)};
    T max_swap_frac{T(1.0)};
    uint64_t dustswap_freq_s{3600};
    bool dustswap_random{false};
    uint64_t dustswap_dynamic_freq_s{0};
    bool dustswap_dynamic_gap_enabled{false};
    T dustswap_dynamic_gap_bps{T(0)};
    uint64_t dustswap_dynamic_heartbeat_s{0};
    uint64_t dustswap_commit_clock_freq_s{0};
    bool policy_keeper_enabled{false};
    bool allow_hybrid_keeper{false};
    uint64_t user_swap_freq_s{0};
    T user_swap_size_frac{T(0.01)};
    T user_swap_thresh{T(0.05)};
    bool save_actions{false};

    // Detailed per-event logging
    bool detailed_log{false};
    size_t detailed_interval{1};  // log every N-th event (1 = all)

    // Optional YieldBasis 2L model. yb_mode selects off / passive /
    // active_2l; fee and cash multiplier configure whichever mode is on.
    YbMode yb_mode{YbMode::Off};
    T yb_releverage_fee{T(0.012)};
    T yb_cash_multiplier{T(1)};

    // Slippage probe sampling
    bool enable_slippage_probes{true};

};

} // namespace harness
} // namespace arb
