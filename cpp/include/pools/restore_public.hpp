#pragma once
#include "pools/pool_init.hpp"
#include "pools/twocrypto_fx/twocrypto.hpp"

namespace arb::pools {
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
}
template<class T>
auto pool_from_public_state(const PoolInit<T>& init) {
    twocrypto_fx::TwoCryptoPool<T> pool(init.precisions, init.A, init.gamma,
        init.mid_fee, init.out_fee, init.fee_gamma, init.adjustment_step_min,
        init.adjustment_step_max, init.ma_time, init.historical_state.price_scale,
        init.reserved_profit_fraction, init.admin_fee, init.policy_kind, init.policy_config);
    pool.donation_duration = init.donation_duration;
    pool.set_block_timestamp(init.historical_state.source_timestamp);
    restore_public_state(pool, init.historical_state);
    return pool;
}
} // namespace arb::pools
