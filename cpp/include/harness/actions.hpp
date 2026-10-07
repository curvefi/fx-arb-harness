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
    std::array<T, 2> balances_after{T(0), T(0)};
    T D_after{0};
    T lp_xcp_profit_after{0};
    T donation_shares_after{0};
    T total_supply_after{0};
    T virtual_price_after{0};  // cached virtual_price (vp_after is the donation-boosted vp)
    T last_donation_release_ts_after{0};
    T donation_protection_expiry_ts_after{0};
};

// A logged active_2l fill (its index), its input and output, and the public
// pool state it left.
template <typename T>
struct InjectedLog {
    uint64_t ts{0};
    uint64_t index{0};
    std::array<T, 2> out{T(0), T(0)};
    std::array<T, 2> balances{T(0), T(0)};
    T D{0};
    T total_supply{0};
    T price_scale{0};
    T price_oracle{0};
    T last_prices{0};
    uint64_t last_timestamp{0};
    T virtual_price{0};
    T xcp_profit{0};
    T lp_xcp_profit{0};
    T donation_shares{0};
    T last_donation_release_ts{0};
    T donation_protection_expiry_ts{0};
    // The LEVAMM state the fill left.
    uint8_t yb_direction{0};  // 0 LP bought with coin0, 1 LP sold
    T yb_collateral{0};
    T yb_debt{0};
    T yb_stable_balance{0};
    T yb_price{0};  // LEVAMM get_p
    T yb_donation{0};
};

// Variant for all action types
template <typename T>
using Action = std::variant<
    DonationAction<T>, TickAction<T>, ExchangeAction<T>, InjectedLog<T>
>;

} // namespace harness
} // namespace arb
