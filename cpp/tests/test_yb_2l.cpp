#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <tuple>
#include <utility>
#include "events/types.hpp"
#include "harness/event_loop.hpp"
#include "harness/run_config.hpp"
#include "harness/yb_2l.hpp"
#include "pools/twocrypto_fx/twocrypto.hpp"
#include "trading/costs.hpp"

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

// Deterministic hourly price walk over 92 days (±10% sine, 12-day period).
// The arb is volume-capped so the pool lags CEX and the 2L model actually
// fires; the walk and cap are identical for every mode under test.
arb::EventSoA make_events(const Pool& pool) {
    arb::EventSoA events;
    constexpr size_t N_EVENTS = 92 * 24;
    const double base = pool.get_p();
    events.ts.reserve(N_EVENTS);
    events.p_cex.reserve(N_EVENTS);
    events.volume.reserve(N_EVENTS);
    events.candle_idx.reserve(N_EVENTS);
    for (size_t k = 0; k < N_EVENTS; ++k) {
        events.ts.push_back(TS + k * 3600ULL);
        constexpr double TWO_PI = 6.283185307179586476925286766559;
        const double phase = TWO_PI * static_cast<double>(k) / 288.0;
        events.p_cex.push_back(base * (1.0 + 0.10 * std::sin(phase)));
        events.volume.push_back(1'000'000.0);
        events.candle_idx.push_back(0);
    }
    return events;
}

struct RunOutcome {
    Pool pool;
    arb::harness::EventLoopResult<double> result;
};

RunOutcome run_with_mode(
    arb::harness::YbMode mode,
    const arb::EventSoA& events
) {
    Pool pool = make_pool();
    arb::harness::RunConfig<double> cfg;
    cfg.yb_mode = mode;
    cfg.dustswap_freq_s = 0;  // no keepers: arb is the only primary mutator
    arb::harness::DonationCfg<double> dcfg{};
    arb::harness::IdleTickCfg<double> icfg{};
    arb::harness::UserSwapCfg<double> ucfg{};
    arb::trading::Costs<double> costs{};
    costs.use_volume_cap = true;
    // Cap arb to ~0.2 coin1 per event: well below the ~1.2 coin1/event CEX
    // drift of the price walk, so the pool lags CEX and the 2L model fires.
    costs.volume_cap_mult = 2e-7;
    auto result = arb::harness::run_event_loop(
        pool, events, costs, dcfg, icfg, ucfg, cfg,
        nullptr, 0,
        static_cast<std::vector<arb::harness::Action<double>>*>(nullptr),
        static_cast<std::vector<arb::harness::DetailedEntry<double>>*>(nullptr)
    );
    return RunOutcome{std::move(pool), std::move(result)};
}

} // namespace

int main() {
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

    // ---- YieldBasis mode event-loop checks: off / active_2l ----
    {
        const Pool probe = make_pool();
        const arb::EventSoA events = make_events(probe);
        auto off = run_with_mode(arb::harness::YbMode::Off, events);
        auto active = run_with_mode(arb::harness::YbMode::Active2l, events);

        // off = nothing: the yb metric family stays empty.
        require(!off.result.yb_releverage_enabled,
                "off mode must not enable YieldBasis");
        require(off.result.yb_releverage_apy == -1.0,
                "off mode must leave yb apy empty");
        require(off.result.yb_releverage_fee == 0.0,
                "off mode must leave yb fee zero");

        // active_2l remains the coherent state-mutating path.
        require(
            !same_pool_state(off.pool, active.pool),
            "active_2l must mutate the pool relative to the off world");
        require(active.result.yb_releverage_enabled,
                "active_2l must enable YieldBasis");
        require(active.result.yb_releverage_fee == 0.012,
                "active_2l must report the configured yb fee");
    }

    // Both enabled modes evaluate every event and produce endpoint APY.
    {
        const Pool probe = make_pool();
        arb::EventSoA events;
        events.ts = {
            TS, TS + 3599, TS + 3600, TS + 3600, TS + 7200, TS + 7201
        };
        events.p_cex.assign(events.ts.size(), probe.get_p() * 1.05);
        events.volume.assign(events.ts.size(), 1'000'000.0);
        events.candle_idx.assign(events.ts.size(), 0);

        const auto active = run_with_mode(
            arb::harness::YbMode::Active2l, events
        );
        const auto reference = run_with_mode(
            arb::harness::YbMode::Reference2l, events
        );
        require(active.result.metrics.yb_2l_attempts == events.size(),
                "active_2l must evaluate every causal event");
        require(reference.result.metrics.yb_2l_attempts == events.size(),
                "reference_2l must evaluate every causal event");
        require(active.result.yb_releverage_apy != -1.0,
                "final endpoint valuation must produce active_2l APY");
        require(reference.result.yb_releverage_apy != -1.0,
                "final endpoint valuation must produce reference_2l APY");
    }

    std::puts("YieldBasis 2L routes + mode/reporting checks: OK");
    return 0;
}
