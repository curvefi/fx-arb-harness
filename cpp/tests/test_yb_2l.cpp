#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <tuple>
#include "harness/yb_2l.hpp"
#include "pools/twocrypto_fx/twocrypto.hpp"

namespace fx = arb::pools::twocrypto_fx;
namespace harness = arb::harness;

using Pool = fx::TwoCryptoPool<double>;
using Actor = harness::Yb2LActor<double>;

namespace {

constexpr uint64_t TS = 1'779'753'600;

Pool make_pool() {
    Pool pool(
        {1.0, 1.0}, 50'000.0, 1.1111111111e-8,
        0.0146, 0.0170, 0.054202748,
        1e-10, 5e-3, 865.0, 77'235.68,
        0.301010101, 0.0
    );
    pool.set_block_timestamp(TS - 1);
    pool.add_liquidity({43'596'754.65, 564.46165}, 0.0);
    pool.set_block_timestamp(TS);
    return pool;
}

harness::YbInitialState<double> historical_yb_state() {
    harness::YbInitialState<double> state;
    state.source_block = 25'455'433;
    state.source_timestamp = 1'783'123'199;
    state.block_hash = "0x75c35faef49be034f515fbfd273a1f1213c78bbaa516fbda170c901ea3cfc7b1";
    state.leverage = 2.0;
    state.fee = 0.013;
    state.collateral = 168'021.7402032929;
    state.debt = 44'446'693.206598505;
    state.rate = 2.59443752e-10;
    state.rate_mul = 1.000887149349038;
    state.rate_time = 1'783'111'751;
    state.minted = 56'657'739.06125437;
    state.redeemed = 12'211'045.854655864;
    state.stable_balance = 40'632'546.70310546;
    state.lt_stable_balance = 0.0;
    state.stable_aggregator = 0.9999390559313684;
    state.lt_donation_discount = 0.01;
    return state;
}

bool same_pool_state(const Pool& lhs, const Pool& rhs) {
    return std::tie(
        lhs.balances,
        lhs.admin_balances,
        lhs.D,
        lhs.totalSupply,
        lhs.cached_price_scale,
        lhs.cached_price_oracle,
        lhs.last_prices,
        lhs.virtual_price,
        lhs.xcp_profit,
        lhs.lp_xcp_profit,
        lhs.donation_shares,
        lhs.last_donation_release_ts,
        lhs.donation_protection_expiry_ts,
        lhs.donation_protection_extension_remainder,
        lhs.last_timestamp,
        lhs.last_admin_fee_claim_timestamp,
        lhs.cached_ema_dt,
        lhs.cached_ema_alpha,
        lhs.cached_ema_alpha_valid,
        lhs.block_timestamp
    ) == std::tie(
        rhs.balances,
        rhs.admin_balances,
        rhs.D,
        rhs.totalSupply,
        rhs.cached_price_scale,
        rhs.cached_price_oracle,
        rhs.last_prices,
        rhs.virtual_price,
        rhs.xcp_profit,
        rhs.lp_xcp_profit,
        rhs.donation_shares,
        rhs.last_donation_release_ts,
        rhs.donation_protection_expiry_ts,
        rhs.donation_protection_extension_remainder,
        rhs.last_timestamp,
        rhs.last_admin_fee_claim_timestamp,
        rhs.cached_ema_dt,
        rhs.cached_ema_alpha,
        rhs.cached_ema_alpha_valid,
        rhs.block_timestamp
    );
}

void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "YieldBasis 2L test failed: %s\n", message);
        std::fflush(stderr);
        std::exit(1);
    }
}

} // namespace

int main() {
    {
        const auto initial = historical_yb_state();
        const auto actor = Actor::from_state(initial);
        require(actor.state().collateral == initial.collateral,
                "active_2l did not retain historical collateral");
        require(actor.state().debt == initial.debt && actor.state().rate == initial.rate &&
                    actor.state().rate_time == initial.rate_time,
                "active_2l did not retain stored debt bookkeeping");
        require(std::fabs(actor.state().lev_ratio - 4.0 / 9.0) < 1e-15 &&
                    actor.state().min_safe_debt_ratio == 1.0 / 16.0 &&
                    actor.state().max_safe_debt_ratio == 17.0 / 32.0,
                "historical leverage did not derive the deployed safety ratios");
        require(actor.projected_debt(initial.source_timestamp) + initial.redeemed > initial.minted,
                "historical state lost its pending interest");

        auto zero_cash = initial;
        zero_cash.stable_balance = 0.0;
        (void)Actor::from_state(zero_cash);

        auto invalid = initial;
        invalid.rate_time = initial.source_timestamp + 1;
        bool rejected = false;
        try {
            (void)Actor::from_state(invalid);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected, "future historical rate_time was accepted");
    }

    bool checked = false;
    for (double relative_price : {0.50, 0.65, 0.80, 1.20, 1.40, 1.70}) {
        Pool pool = make_pool();
        const Pool pool_before = pool;
        auto actor = Actor::fresh_2l(pool, 0.0145, 0.012, TS, 3.0);
        const auto actor_state_before = actor.state();
        const auto result = actor.try_fire(
            pool,
            pool.get_p() * relative_price,
            TS + 86400,
            Actor::Costs{}
        );
        if (!result.fired) {
            require(
                same_pool_state(pool, pool_before),
                "a non-fired route must not mutate the pool"
            );
            require(
                actor.state().stable_balance ==
                    actor_state_before.stable_balance,
                "a non-fired route must not mutate stable balance"
            );
            continue;
        }

        require(result.donation_committed, "a fired route must commit its real donation");
        require(result.donation > 0.0, "committed donation must be positive");
        require(
            result.fill_adds + result.fill_removes > 0,
            "real-leg route must record a proportional pool fill"
        );
        require(
            pool.donation_shares > pool_before.donation_shares,
            "committed donation must increase pool donation shares"
        );
        require(
            std::fabs(actor.state().collateral - (pool.totalSupply - pool.donation_shares
                    - fx::PoolTraits<double>::MINIMUM_LIQUIDITY())) <=
                1e-6 * std::max(1.0, std::fabs(actor.state().collateral)),
            "real-leg fill must leave collateral equal to the circulating LP"
        );
        require(
            actor.state().stable_balance != actor_state_before.stable_balance,
            "a fired route must commit its cash leg"
        );
        checked = true;
        break;
    }

    require(checked, "test inputs must produce a cash3 atomic real-leg fill");

    // Around an equilibrium the no-trade band holds the price, and at its edges
    // neither the gate nor the actor acts.
    {
        Pool pool = make_pool();
        auto actor = Actor::fresh_2l(pool, 0.0145, 0.012, TS, 3.0);
        pool.set_block_timestamp(TS + 3600);
        Actor::Costs costs;
        const double price = pool.get_p();
        const auto band = actor.no_trade_band(pool, price, TS + 3600, TS + 7200, costs);
        require(band.valid && band.lo < price && price < band.hi, "equilibrium price outside the no-trade band");
        for (double p : {band.lo, band.hi}) for (uint64_t ts : {TS + 3600, TS + 7200}) {
            Pool trial = pool;
            auto trial_actor = actor;
            require(!actor.may_trade(pool, p, ts, costs) && !trial_actor.try_fire(trial, p, ts, costs).fired &&
                        same_pool_state(trial, pool), "the gate or actor acted inside its band");
        }
    }

    std::puts("YieldBasis 2L atomic route checks: OK");
    return 0;
}
