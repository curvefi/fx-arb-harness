#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <tuple>
#include <utility>
#include "harness/yb_2l.hpp"
#include "harness/yb_reference_2l.hpp"
#include "pools/twocrypto_fx/twocrypto.hpp"

namespace fx = arb::pools::twocrypto_fx;
namespace harness = arb::harness;

using Pool = fx::TwoCryptoPool<double>;
using Actor = harness::Yb2LActor<double>;
using ReferenceMarket = harness::YbReference2LMarket<double>;

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
    state.flash_max_loan = 30'000'000.379967466;
    state.stable_aggregator = 0.9999390559313684;
    state.rounding_discount = 1e-8;
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
        const auto market = ReferenceMarket::from_state(initial);
        require(actor.state().collateral == initial.collateral,
                "active_2l did not retain historical collateral");
        require(actor.state().debt == initial.debt && actor.state().rate == initial.rate &&
                    actor.state().rate_time == initial.rate_time,
                "active_2l did not retain stored debt bookkeeping");
        require(std::fabs(actor.state().lev_ratio - 4.0 / 9.0) < 1e-15 &&
                    actor.state().min_safe_debt_ratio == 1.0 / 16.0 &&
                    actor.state().max_safe_debt_ratio == 17.0 / 32.0,
                "historical leverage did not derive the deployed safety ratios");
        require(market.state().flash_max_loan == initial.flash_max_loan &&
                    market.state().stable_aggregator == initial.stable_aggregator &&
                    market.state().rounding_discount == initial.rounding_discount,
                "reference_2l did not retain historical external inputs");
        const auto checkpoint_interest =
            actor.projected_interest_summary(initial.source_timestamp);
        require(std::fabs(checkpoint_interest.accrued) < 1e-9 &&
                    checkpoint_interest.pending_interest > 0.0 &&
                    std::fabs(checkpoint_interest.conservation_residual) < 1e-6,
                "historical state lost initial pending-interest conservation");
        const auto future_interest =
            actor.projected_interest_summary(initial.source_timestamp + 86400);
        require(future_interest.accrued > 0.0 &&
                    std::fabs(future_interest.conservation_residual) < 1e-6,
                "post-checkpoint interest accounting did not conserve");

        auto zero_cash = initial;
        zero_cash.stable_balance = 0.0;
        zero_cash.flash_max_loan = 0.0;
        (void)Actor::from_state(zero_cash);
        (void)ReferenceMarket::from_state(zero_cash);

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

    // Contract-derived VirtualPool route surface: both directions commit their
    // required native legs, while a forced final-output failure rolls back the
    // complete pool and YB state.
    for (size_t direction = 0; direction < 2; ++direction) {
        Pool pool = make_pool();
        const Pool pool_before = pool;
        auto market = ReferenceMarket::fresh_2l(
            pool, 0.0145, 0.012, TS, 3.0
        );
        const auto state_before = market.state();
        const double basis = pool.balances[direction];
        harness::YbReference2LRouteResult<double> route;
        for (double fraction : {1e-7, 1e-6, 1e-5, 1e-4, 1e-3}) {
            Pool candidate_pool = pool_before;
            auto candidate_market = ReferenceMarket::fresh_2l(
                candidate_pool, 0.0145, 0.012, TS, 3.0
            );
            route = candidate_market.apply_atomic(
                candidate_pool, direction, basis * fraction, 0.0, TS + 3600
            );
            if (route.committed) {
                pool = std::move(candidate_pool);
                market = std::move(candidate_market);
                break;
            }
        }
        require(route.committed, "reference_2l must execute both route directions");
        require(route.output > 0.0, "reference_2l route output must be positive");
        require(
            direction == 0 ? route.emitted_remove : route.emitted_add,
            "reference_2l must execute the direction-specific native leg"
        );
        require(
            !same_pool_state(pool, pool_before),
            "reference_2l committed route must mutate the pool"
        );

        Pool failed_pool = pool_before;
        auto failed_market = ReferenceMarket::fresh_2l(
            failed_pool, 0.0145, 0.012, TS, 3.0
        );
        const auto failed = failed_market.apply_atomic(
            failed_pool, direction, route.input,
            std::nextafter(route.output, std::numeric_limits<double>::infinity()),
            TS + 3600
        );
        require(!failed.committed, "reference_2l forced slippage must reject");
        require(
            same_pool_state(failed_pool, pool_before),
            "reference_2l rejected route must roll back the complete pool"
        );
        require(
            failed_market.state().collateral == state_before.collateral &&
            failed_market.state().debt == state_before.debt &&
            failed_market.state().stable_balance == state_before.stable_balance,
            "reference_2l rejected route must roll back YB state"
        );
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
                actor.shadow_violations() == 0,
                "a non-fired route must not record shadow-ledger violations"
            );
            require(
                same_pool_state(pool, pool_before),
                "a non-fired route must not mutate the pool"
            );
            require(
                actor.state().stable_balance ==
                    actor_state_before.stable_balance,
                "a non-fired route must not mutate stable balance"
            );
            require(
                pool.donation_shares == pool_before.donation_shares,
                "a non-fired route must not mutate donation shares"
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
            actor.shadow_violations() == 0,
            "cash3 atomic real legs must preserve the shadow-ledger invariant"
        );
        require(
            actor.shadow_checks() > 0,
            "a fired route must check the shadow-ledger invariant"
        );
        require(
            std::fabs(actor.shadow_gap_max()) <=
                1e-6 * std::max(1.0, std::fabs(actor.state().collateral)),
            "real-leg fill must leave no material shadow-ledger gap"
        );
        require(
            actor.state().stable_balance != actor_state_before.stable_balance,
            "a fired route must commit its cash leg"
        );
        checked = true;
        break;
    }

    require(checked, "test inputs must produce a cash3 atomic real-leg fill");

    std::puts("YieldBasis 2L atomic route checks: OK");
    return 0;
}
