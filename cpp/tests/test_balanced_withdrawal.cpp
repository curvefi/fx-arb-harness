#include <array>
#include <cstdio>
#include <iostream>
#include <stdexcept>

#include "pools/twocrypto_fx/twocrypto.hpp"

namespace fx = arb::pools::twocrypto_fx;
using Pool = fx::TwoCryptoPool<double>;

namespace {

Pool make_pool() {
    Pool pool(
        {1.0, 1.0}, 50000.0, 1e-4, 0.001, 0.002, 0.01,
        1e-10, 5e-3, 865.0, 1.0, 0.5, 0.5,
        fx::PolicyKind::Compiled
    );
    pool.add_liquidity({1'000'000.0, 1'000'000.0}, 0.0);
    return pool;
}

bool same_pool_state(const Pool& a, const Pool& b) {
    return a.balances == b.balances &&
        a.admin_balances == b.admin_balances &&
        a.D == b.D &&
        a.totalSupply == b.totalSupply &&
        a.cached_price_scale == b.cached_price_scale &&
        a.cached_price_oracle == b.cached_price_oracle &&
        a.last_prices == b.last_prices &&
        a.virtual_price == b.virtual_price &&
        a.xcp_profit == b.xcp_profit &&
        a.lp_xcp_profit == b.lp_xcp_profit &&
        a.donation_shares == b.donation_shares &&
        a.last_donation_release_ts == b.last_donation_release_ts &&
        a.donation_protection_expiry_ts == b.donation_protection_expiry_ts &&
        a.donation_protection_extension_remainder ==
            b.donation_protection_extension_remainder &&
        a.last_timestamp == b.last_timestamp &&
        a.last_admin_fee_claim_timestamp ==
            b.last_admin_fee_claim_timestamp &&
        a.cached_ema_dt == b.cached_ema_dt &&
        a.cached_ema_alpha == b.cached_ema_alpha &&
        a.cached_ema_alpha_valid == b.cached_ema_alpha_valid &&
        a.block_timestamp == b.block_timestamp &&
        a.policy.compiled_state.update_count ==
            b.policy.compiled_state.update_count &&
        a.policy.compiled_state.throw_on_update ==
            b.policy.compiled_state.throw_on_update;
}

bool test_mutable_snapshot_restores_every_field() {
    Pool pool = make_pool();
    const Pool before = pool;
    const auto snapshot = pool.mutable_snapshot();

    pool.balances = {
        before.balances[0] + 101.0,
        before.balances[1] + 202.0,
    };
    pool.admin_balances = {
        before.admin_balances[0] + 303.0,
        before.admin_balances[1] + 404.0,
    };
    pool.D = before.D + 505.0;
    pool.totalSupply = before.totalSupply + 606.0;
    pool.cached_price_scale = before.cached_price_scale + 707.0;
    pool.cached_price_oracle = before.cached_price_oracle + 808.0;
    pool.last_prices = before.last_prices + 909.0;
    pool.virtual_price = before.virtual_price + 1'010.0;
    pool.xcp_profit = before.xcp_profit + 1'111.0;
    pool.lp_xcp_profit = before.lp_xcp_profit + 1'212.0;
    pool.donation_shares = before.donation_shares + 1'313.0;
    pool.last_donation_release_ts = before.last_donation_release_ts + 1'414.0;
    pool.donation_protection_expiry_ts =
        before.donation_protection_expiry_ts + 1'515.0;
    pool.donation_protection_extension_remainder =
        before.donation_protection_extension_remainder + 1'616.0;
    pool.last_timestamp = before.last_timestamp + 1'717;
    pool.last_admin_fee_claim_timestamp =
        before.last_admin_fee_claim_timestamp + 1'718;
    pool.cached_ema_dt = before.cached_ema_dt + 1'818;
    pool.cached_ema_alpha = before.cached_ema_alpha + 1'919.0;
    pool.cached_ema_alpha_valid = !before.cached_ema_alpha_valid;
    pool.block_timestamp = before.block_timestamp + 2'020;

    if (same_pool_state(pool, before)) {
        std::printf("mutable snapshot mutation setup was ineffective\n");
        return false;
    }

    pool.restore_mutable(snapshot);
    if (!same_pool_state(pool, before)) {
        std::printf("mutable snapshot failed to restore every field\n");
        return false;
    }
    return true;
}

bool test_balanced_withdrawal() {
    Pool pool = make_pool();
    const double supply_before = pool.totalSupply;
    const double d_before = pool.D;
    const std::array<double, 2> balances_before = pool.balances;
    const double amount = supply_before / 4.0;
    const std::array<double, 2> expected{
        balances_before[0] * amount / supply_before,
        balances_before[1] * amount / supply_before,
    };

    const auto withdrawn = pool.remove_liquidity(amount, {0.0, 0.0});
    if (withdrawn != expected ||
        pool.totalSupply != supply_before - amount ||
        pool.D != d_before - (d_before * amount / supply_before) ||
        pool.balances[0] != balances_before[0] - expected[0] ||
        pool.balances[1] != balances_before[1] - expected[1]) {
        std::printf("balanced withdrawal changed the wrong state\n");
        return false;
    }
    return true;
}

bool test_slippage_reverts_withdrawal() {
    Pool pool = make_pool();
    const Pool before = pool;
    const double amount = pool.totalSupply / 4.0;
    const double expected_coin0 = pool.balances[0] * amount / pool.totalSupply;

    try {
        (void)pool.remove_liquidity(amount, {expected_coin0 + 1.0, 0.0});
        std::printf("balanced withdrawal ignored slippage failure\n");
        return false;
    } catch (const std::runtime_error&) {
    }

    if (!same_pool_state(pool, before)) {
        std::printf("slippage failure mutated pool state\n");
        return false;
    }
    return true;
}

bool test_policy_exception_reverts_add() {
    Pool pool = make_pool();
    pool.set_block_timestamp(1);
    pool.policy.compiled_state.throw_on_update = true;
    const Pool before = pool;

    try {
        (void)pool.add_liquidity({10'000.0, 10'000.0}, 0.0);
        std::printf("compiled-policy add failure did not propagate\n");
        return false;
    } catch (const std::runtime_error&) {
    }

    if (!same_pool_state(pool, before)) {
        std::printf("compiled-policy add failure mutated pool state\n");
        return false;
    }
    return true;
}

bool test_policy_exception_reverts_exchange() {
    Pool pool = make_pool();
    pool.set_block_timestamp(1);
    pool.policy.compiled_state.throw_on_update = true;
    const Pool before = pool;

    try {
        (void)pool.exchange(0.0, 1.0, 10'000.0, 0.0);
        std::printf("compiled-policy exchange failure did not propagate\n");
        return false;
    } catch (const std::runtime_error&) {
    }

    if (!same_pool_state(pool, before)) {
        std::printf("compiled-policy exchange failure mutated pool state\n");
        return false;
    }
    return true;
}

bool test_policy_exception_reverts_fixed_out_and_output() {
    Pool pool = make_pool();
    pool.set_block_timestamp(100'000);
    pool.policy.compiled_state.throw_on_update = true;
    const Pool before = pool;
    double charged_lp_fee = 123.0;

    try {
        (void)pool.remove_liquidity_fixed_out(
            pool.totalSupply / 100.0,
            0,
            0.0,
            0.0,
            &charged_lp_fee
        );
        std::printf("compiled-policy fixed-out failure did not propagate\n");
        return false;
    } catch (const std::runtime_error&) {
    }

    if (!same_pool_state(pool, before) || charged_lp_fee != 123.0) {
        std::printf("compiled-policy fixed-out failure leaked state or output\n");
        return false;
    }
    return true;
}

} // namespace

int main() {
    if (!test_mutable_snapshot_restores_every_field() ||
        !test_balanced_withdrawal() ||
        !test_slippage_reverts_withdrawal() ||
        !test_policy_exception_reverts_add() ||
        !test_policy_exception_reverts_exchange() ||
        !test_policy_exception_reverts_fixed_out_and_output()) {
        return 1;
    }
    std::cout << "test_balanced_withdrawal: PASSED\n";
    return 0;
}
