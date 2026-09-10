// Action recording value types for saved action traces (transport-free).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <variant>
#include <vector>
#include <string>


namespace arb {
namespace harness {

struct ReconciliationSummary {
    uint64_t observations{}, episodes{}, resets{};
};
template<class T> struct StateReconciliationAction {
    std::string phase;
    uint64_t wall_ns{}, source_block{}, source_timestamp{}, available_ns{};
    uint64_t detection_ns{}, deadline_ns{}, apply_ns{}, segment{};
    bool state_changed{false};
    std::array<T, 2> balances_before{}, balances_after{};
    T scale_before{}, scale_after{}, debt_before{}, debt_after{};
    T collateral_before{}, collateral_after{}, cash_before{}, cash_after{};
    T vp_before{}, vp_after{}, xcp_before{}, xcp_after{};
};

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
    bool synthetic_user{false};
    uint64_t ts{0};
    int i{0};
    int j{0};
    T dx{0};
    T dy_after_fee{0};
    T fee_tokens{0};
    T profit_coin0{0};
    T p_cex{0};
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

// direction 0 consumes coin0 and returns coin1; direction 1 consumes coin1
// and returns coin0. Profit, donation, and flash_amount are coin0 quantities.
template <typename T>
struct YbRouteAction {
    uint64_t ts{0};
    size_t direction{0};
    T input{0};
    T output{0};
    T profit_coin0{0};
    T lp_amount{0};
    T donation{0};
    T flash_amount{0};
};

// Variant for all action types
template <typename T>
using Action = std::variant<
    DonationAction<T>, TickAction<T>, ExchangeAction<T>, YbRouteAction<T>,
    StateReconciliationAction<T>
>;

} // namespace harness
} // namespace arb
