// Action recording value types for saved action traces (transport-free).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <variant>
#include <vector>

namespace arb {
namespace harness {

// Donation action
template <typename T>
struct DonationAction {
    uint64_t ts{0};
    uint64_t ts_due{0};
    std::array<T, 2> amounts{T(0), T(0)};
    T price_scale{0};
    T donation_ratio1{0};
    T apy_per_year{0};
    uint64_t freq_s{0};
};

// Tick action
template <typename T>
struct TickAction {
    uint64_t ts{0};
    T p_cex{0};
    T ps_before{0};
    T ps_after{0};
    T oracle_before{0};
    T oracle_after{0};
    T xcp_profit_before{0};
    T xcp_profit_after{0};
    T vp_before{0};
    T vp_after{0};
};

// Exchange action
template <typename T>
struct ExchangeAction {
    uint64_t ts{0};
    int i{0};
    int j{0};
    T dx{0};
    T dy_after_fee{0};
    T fee_tokens{0};
    T profit_coin0{0};
    T p_cex{0};
    T implied_fair{0};
    T floor_implied_fair{0};
    T p_pool_before{0};
    T p_pool_after{0};
    T oracle_before{0};
    T oracle_after{0};
    T ps_before{0};
    T ps_after{0};
    uint64_t last_ts_before{0};
    uint64_t last_ts_after{0};
    T lp_before{0};
    T lp_after{0};
    T xcp_profit_before{0};
    T xcp_profit_after{0};
    T vp_before{0};
    T vp_after{0};
    T balance_indicator{0};
};

// Variant for all action types
template <typename T>
using Action = std::variant<
    DonationAction<T>, TickAction<T>, ExchangeAction<T>
>;

} // namespace harness
} // namespace arb
