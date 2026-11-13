#pragma once
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "pools/pool_init.hpp"
#include "pools/twocrypto_fx/twocrypto.hpp"

namespace arb::pools {
namespace restore_detail {
template <class S, class = void>
struct IsDualEmaState : std::false_type {};
template <class S>
struct IsDualEmaState<S, std::void_t<
    decltype(std::declval<S&>().last_update_ts), decltype(std::declval<S&>().last_prices),
    decltype(std::declval<S&>().fast_ema), decltype(std::declval<S&>().slow_ema),
    decltype(std::declval<S&>().price_scale)>> : std::true_type {};
template <class S, class = void>
struct HasPolicyGeometry : std::false_type {};
template <class S>
struct HasPolicyGeometry<S, std::void_t<
    decltype(std::declval<S&>().xp), decltype(std::declval<S&>().D), decltype(std::declval<S&>().spot)>> : std::true_type {};
template <class S, class = void>
struct HasPolicyReport : std::false_type {};
template <class S>
struct HasPolicyReport<S, std::void_t<
    decltype(std::declval<S&>().report.price), decltype(std::declval<S&>().report.timestamp)>> : std::true_type {};
} // namespace restore_detail

// The chain's dual-EMA policy fields replace the ones initialized from the pool; the committed geometry and the
// stored report, when the checkpoint carries them, replace the pool-derived geometry and the empty report. The spot
// is derived from the committed geometry as the policy's own update does.
template<class T, class Pool>
void restore_policy_state(Pool& pool, const PoolHistoricalState<T>& state) {
#ifdef TWOCRYPTO_POLICY_HEADER
    using State = std::decay_t<decltype(pool.policy.compiled_state)>;
    if constexpr (restore_detail::IsDualEmaState<State>::value) {
        if (pool.policy.kind == twocrypto_fx::PolicyKind::Compiled) {
            State& restored = pool.policy.compiled_state;
            restored.last_update_ts = state.policy_last_update_ts;
            restored.last_prices = state.policy_last_prices;
            restored.fast_ema = state.policy_fast_ema;
            restored.slow_ema = state.policy_slow_ema;
            restored.price_scale = state.policy_price_scale;
            if constexpr (restore_detail::HasPolicyGeometry<State>::value) {
                if (state.has_policy_geometry) {
                    restored.xp = state.policy_xp;
                    restored.D = state.policy_D;
                }
                restored.spot = restored.D > T(0) && restored.xp[0] > T(0) && restored.xp[1] > T(0)
                    ? twocrypto_fx::MathOps<T>::get_p(restored.xp, restored.D, {pool.policy.config.A, T(0)}) * restored.price_scale
                    : T(0);
            } else if (state.has_policy_geometry) {
                throw std::invalid_argument("historical policy_state xp and D need a policy with committed geometry");
            }
            if constexpr (restore_detail::HasPolicyReport<State>::value) {
                if (state.has_policy_report) {
                    // The report may postdate the checkpoint (delivered later without a pool touch) but not the run start.
                    if (state.policy_report_timestamp > static_cast<double>(pool.block_timestamp) + 6.0)
                        throw std::invalid_argument("historical policy_state report observed after the run start");
                    restored.report.price = state.policy_report_price;
                    restored.report.timestamp = state.policy_report_timestamp;
                }
            } else if (state.has_policy_report) {
                throw std::invalid_argument("historical policy_state report needs a report policy");
            }
            return;
        }
    }
#endif
    (void)pool;
    (void)state;
    throw std::invalid_argument("historical policy_state requires a compiled dual-EMA policy");
}
template<class T, class Pool>
void restore_public_state(Pool& pool, const PoolHistoricalState<T>& state) {
    pool.balances = state.balances;
    pool.admin_balances = state.admin_balances;
    pool.last_admin_fee_claim_timestamp =
        state.last_admin_fee_claim_timestamp;
    pool.D = state.D;
    pool.totalSupply = state.total_supply;
    pool.cached_price_scale = state.price_scale;
    pool.cached_price_oracle = state.price_oracle;
    pool.last_prices = state.last_prices;
    pool.last_timestamp = state.last_timestamp;
    pool.virtual_price = state.virtual_price;
    pool.xcp_profit = state.xcp_profit;
    pool.lp_xcp_profit = state.lp_xcp_profit;
    pool.donation_shares = state.donation_shares;
    pool.last_donation_release_ts = state.last_donation_release_ts;
    pool.donation_protection_expiry_ts = state.donation_protection_expiry_ts;
    pool.donation_protection_period = state.donation_protection_period;
    pool.donation_protection_lp_threshold = state.donation_protection_lp_threshold;
    pool.donation_protection_extension_remainder =
        state.donation_protection_extension_remainder;
    pool.donation_shares_max_ratio = state.donation_shares_max_ratio;
    pool.cached_ema_dt = 0;
    pool.cached_ema_alpha = T(0);
    pool.cached_ema_alpha_valid = false;
    pool.initialize_policy_state_from_pool();
    if (state.has_policy_state) restore_policy_state(pool, state);
}
} // namespace arb::pools
