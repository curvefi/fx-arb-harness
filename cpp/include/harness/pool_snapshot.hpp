// Scalar snapshot/rollback of a pool's mutable state.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

#include "pools/twocrypto_fx/policy.hpp"

namespace arb {
namespace harness {

template <typename T>
struct PoolScalarSnapshot {
    std::array<T, 2> balances, admin_balances;
    T D, totalSupply, cached_price_scale, cached_price_oracle;
    T last_prices, virtual_price, xcp_profit, lp_xcp_profit;
    T donation_shares, last_donation_release_ts;
    T donation_protection_expiry_ts, donation_protection_extension_remainder;
    uint64_t last_timestamp, last_admin_fee_claim_timestamp, cached_ema_dt;
    T cached_ema_alpha;
    bool cached_ema_alpha_valid;

    template <typename Pool>
    void capture(const Pool& pool) {
        balances = pool.balances;
        admin_balances = pool.admin_balances;
        D = pool.D;
        totalSupply = pool.totalSupply;
        cached_price_scale = pool.cached_price_scale;
        cached_price_oracle = pool.cached_price_oracle;
        last_prices = pool.last_prices;
        virtual_price = pool.virtual_price;
        xcp_profit = pool.xcp_profit;
        lp_xcp_profit = pool.lp_xcp_profit;
        donation_shares = pool.donation_shares;
        last_donation_release_ts = pool.last_donation_release_ts;
        donation_protection_expiry_ts = pool.donation_protection_expiry_ts;
        donation_protection_extension_remainder = pool.donation_protection_extension_remainder;
        last_timestamp = pool.last_timestamp;
        last_admin_fee_claim_timestamp = pool.last_admin_fee_claim_timestamp;
        cached_ema_dt = pool.cached_ema_dt;
        cached_ema_alpha = pool.cached_ema_alpha;
        cached_ema_alpha_valid = pool.cached_ema_alpha_valid;
    }

    template <typename Pool>
    void restore(Pool& pool) const {
        pool.balances = balances;
        pool.admin_balances = admin_balances;
        pool.D = D;
        pool.totalSupply = totalSupply;
        pool.cached_price_scale = cached_price_scale;
        pool.cached_price_oracle = cached_price_oracle;
        pool.last_prices = last_prices;
        pool.virtual_price = virtual_price;
        pool.xcp_profit = xcp_profit;
        pool.lp_xcp_profit = lp_xcp_profit;
        pool.donation_shares = donation_shares;
        pool.last_donation_release_ts = last_donation_release_ts;
        pool.donation_protection_expiry_ts = donation_protection_expiry_ts;
        pool.donation_protection_extension_remainder = donation_protection_extension_remainder;
        pool.last_timestamp = last_timestamp;
        pool.last_admin_fee_claim_timestamp = last_admin_fee_claim_timestamp;
        pool.cached_ema_dt = cached_ema_dt;
        pool.cached_ema_alpha = cached_ema_alpha;
        pool.cached_ema_alpha_valid = cached_ema_alpha_valid;
    }
};

// Transaction snapshot used by the event loop's speculative exchange/tick
// paths. Native pools mutate only the audited scalar surface above. Unnative
// policy kinds additionally mutate their research clock, policy state, and
// hook telemetry; the pool's PolicyModel owns the exact rollback surface
// (mutable_snapshot/restore_mutable), which also covers the compiled policy
// state. Other policy kinds retain the conservative full-pool fallback.
template <typename T, typename Pool>
class PoolTransactionSnapshot {
    using PolicySnapshot = std::decay_t<decltype(
        std::declval<Pool&>().policy.mutable_snapshot()
    )>;
    using HookMetrics = std::decay_t<decltype(
        std::declval<Pool&>().policy_hook_metrics
    )>;

public:
    PoolTransactionSnapshot(
        const Pool& pool,
        bool scalar_only,
        bool compact_policy
    ) : scalar_only_(scalar_only), compact_policy_(compact_policy) {
        if (scalar_only_ || compact_policy_) {
            scalar_.capture(pool);
        }
        if (compact_policy_) {
            policy_snapshot_.emplace(pool.policy.mutable_snapshot());
            hook_metrics_.emplace(pool.policy_hook_metrics);
        } else if (!scalar_only_) {
            full_ = std::make_unique<Pool>(pool);
        }
    }

    void restore(Pool& pool) const {
        if (full_) {
            pool = *full_;
            return;
        }
        scalar_.restore(pool);
        if (compact_policy_) {
            pool.policy.restore_mutable(*policy_snapshot_);
            pool.policy_hook_metrics = *hook_metrics_;
        }
    }

private:
    bool scalar_only_{false};
    bool compact_policy_{false};
    PoolScalarSnapshot<T> scalar_{};
    std::optional<PolicySnapshot> policy_snapshot_;
    std::optional<HookMetrics> hook_metrics_;
    std::unique_ptr<Pool> full_;
};

} // namespace harness
} // namespace arb
