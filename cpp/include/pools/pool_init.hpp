// Pool initialization value types.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "pools/twocrypto_fx/policy.hpp"

namespace arb {
namespace pools {

// Optional exact scalar state for replaying an already-live pool. All token,
// invariant, price and LP values use the same 1e18-scaled JSON convention as
// the existing pool config. When disabled, the runner keeps its historical
// fresh-pool initialization path exactly.
template <typename T>
struct PoolHistoricalState {
    bool enabled{false};
    uint64_t source_block{0};
    uint64_t source_timestamp{0};

    std::array<T, 2> balances{T(0), T(0)};
    std::array<T, 2> admin_balances{T(0), T(0)};
    uint64_t last_admin_fee_claim_timestamp{0};
    T D{T(0)};
    T total_supply{T(0)};
    T price_scale{T(0)};
    T price_oracle{T(0)};
    T last_prices{T(0)};
    uint64_t last_timestamp{0};
    T virtual_price{T(0)};
    T xcp_profit{T(0)};
    T lp_xcp_profit{T(0)};

    T donation_shares{T(0)};
    T last_donation_release_ts{T(0)};
    T donation_protection_expiry_ts{T(0)};
    T donation_protection_period{T(0)};
    T donation_protection_lp_threshold{T(0)};
    T donation_protection_extension_remainder{T(0)};
    T donation_shares_max_ratio{T(0)};
};

// Pool initialization parameters (floating-point, unit-scaled).
template <typename T>
struct PoolInit {
    std::array<T, 2> precisions{T(1), T(1)};
    T A{T(100000.0)};
    T gamma{T(0)};
    T mid_fee{T(3e-4)};
    T out_fee{T(5e-4)};
    T fee_gamma{T(0.23)};
    T adjustment_step_min{T(1e-6)};
    T adjustment_step_max{T(1e-3)};
    T ma_time{T(600.0)};
    T reserved_profit_fraction{T(0.5)};
    T admin_fee{T(0.5)};
    twocrypto_fx::PolicyKind policy_kind{twocrypto_fx::PolicyKind::None};
    twocrypto_fx::PolicyConfig<T> policy_config{};
    T initial_price{T(1.0)};
    std::array<T, 2> initial_liq{T(1e6), T(1e6)};
    uint64_t start_ts{0};
    PoolHistoricalState<T> historical_state{};

    T donation_apy{T(0)};
    T donation_frequency{T(0)};
    T donation_duration{T(7 * 86400)};
    T initial_donation_days{T(0)};
    T donation_coins_ratio{T(0.5)};
    // Optional per-pool user-swap size (fraction of from-side balance); when
    // unset the run-level RunConfig::user_swap_size_frac applies.
    std::optional<T> user_swap_size_frac{};
    std::string tag;
    size_t global_index{0};
};

} // namespace pools
} // namespace arb
