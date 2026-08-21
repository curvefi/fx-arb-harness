#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>

#include "harness/donation.hpp"
#include "harness/pool_snapshot.hpp"

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct ThrowingPool {
    bool throw_on_add{true};
    double cached_price_oracle{2.0};
    double cached_price_scale{3.0};
    std::array<double, 2> balances{100.0, 50.0};
    std::array<double, 2> admin_balances{7.0, 11.0};
    uint64_t block_timestamp{1234};
    int mutation_counter{19};

    struct MutableSnapshot {
        double cached_price_oracle;
        double cached_price_scale;
        std::array<double, 2> balances;
        std::array<double, 2> admin_balances;
        uint64_t block_timestamp;
        int mutation_counter;
    };

    MutableSnapshot mutable_snapshot() const {
        return {
            cached_price_oracle,
            cached_price_scale,
            balances,
            admin_balances,
            block_timestamp,
            mutation_counter,
        };
    }

    void restore_mutable(const MutableSnapshot& snapshot) {
        cached_price_oracle = snapshot.cached_price_oracle;
        cached_price_scale = snapshot.cached_price_scale;
        balances = snapshot.balances;
        admin_balances = snapshot.admin_balances;
        block_timestamp = snapshot.block_timestamp;
        mutation_counter = snapshot.mutation_counter;
    }

    std::array<double, 2> add_liquidity(
        std::array<double, 2> amounts,
        double,
        bool donation
    ) {
        require(donation, "the scheduler must mark this add as a donation");
        balances[0] += amounts[0];
        balances[1] += amounts[1];
        cached_price_scale = 17.0;
        admin_balances = {91.0, 93.0};
        block_timestamp = 9876;
        mutation_counter = -1;
        if (throw_on_add) {
            throw std::runtime_error("intentional donation failure");
        }
        return amounts;
    }
};

bool same_pool(const ThrowingPool& lhs, const ThrowingPool& rhs) {
    return lhs.cached_price_oracle == rhs.cached_price_oracle &&
           lhs.cached_price_scale == rhs.cached_price_scale &&
           lhs.balances == rhs.balances &&
           lhs.admin_balances == rhs.admin_balances &&
           lhs.block_timestamp == rhs.block_timestamp &&
           lhs.mutation_counter == rhs.mutation_counter &&
           lhs.throw_on_add == rhs.throw_on_add;
}

} // namespace

int main() {
    using T = double;
    using arb::harness::DonationCfg;
    using arb::harness::Metrics;

    DonationCfg<T> high_ratio;
    high_ratio.init(T(0.05), T(3600), T(3), 1000);
    require(high_ratio.enabled, "positive APY and frequency must activate donations");
    require(high_ratio.freq_s == 3600, "frequency must be retained in seconds");
    require(high_ratio.next_ts == 1000, "activation must schedule from the start timestamp");
    require(high_ratio.ratio1 == T(1), "coin ratio must clamp at one");

    DonationCfg<T> low_ratio;
    low_ratio.init(T(0.05), T(3600), T(-3), 1000);
    require(low_ratio.enabled, "positive APY and frequency must activate donations");
    require(low_ratio.ratio1 == T(0), "coin ratio must clamp at zero");

    DonationCfg<T> inactive_apy;
    inactive_apy.init(T(0), T(3600), T(0.5), 1000);
    require(!inactive_apy.enabled, "zero APY must leave donations inactive");

    DonationCfg<T> inactive_frequency;
    inactive_frequency.init(T(0.05), T(0), T(0.5), 1000);
    require(!inactive_frequency.enabled, "zero frequency must leave donations inactive");

    ThrowingPool pool;
    const ThrowingPool pool_before = pool;
    Metrics<T> metrics;
    metrics.n_rebalances = 11;
    metrics.donations = 13;
    metrics.donation_coin0_total = T(17.5);
    metrics.donation_amounts_total = {T(19.5), T(23.5)};
    const auto n_rebalances_before = metrics.n_rebalances;
    const auto donations_before = metrics.donations;
    const auto donation_coin0_before = metrics.donation_coin0_total;
    const auto donation_amounts_before = metrics.donation_amounts_total;

    arb::harness::PoolTransactionSnapshot<ThrowingPool> transaction_snapshot(pool);
    auto result = arb::harness::make_donation_ex(pool, high_ratio, 5000, metrics);
    if (!result.success) {
        transaction_snapshot.restore(pool);
    }

    require(!result.success, "a throwing pool must report a failed donation");
    require(same_pool(pool, pool_before), "failed donation must restore the complete pool");
    require(metrics.n_rebalances == n_rebalances_before,
            "failed donation must not change rebalance metrics");
    require(metrics.donations == donations_before,
            "failed donation must not change donation count");
    require(metrics.donation_coin0_total == donation_coin0_before,
            "failed donation must not change coin0 metrics");
    require(metrics.donation_amounts_total == donation_amounts_before,
            "failed donation must not change amount metrics");
    require(high_ratio.next_ts == 4600,
            "a failed donation must advance exactly one configured period");

    ThrowingPool committing_pool;
    committing_pool.throw_on_add = false;
    DonationCfg<T> committing_cfg;
    committing_cfg.init(T(0.05), T(3600), T(3), 1000);
    Metrics<T> committing_metrics;
    committing_metrics.n_rebalances = 11;
    committing_metrics.donations = 13;
    committing_metrics.donation_coin0_total = T(17.5);
    committing_metrics.donation_amounts_total = {T(19.5), T(23.5)};
    const auto committing_result = arb::harness::make_donation_ex(
        committing_pool,
        committing_cfg,
        5000,
        committing_metrics
    );

    require(committing_result.success,
            "a non-throwing pool must report a successful donation");
    require(committing_result.ts_due == 1000,
            "a successful donation must record the due timestamp");

    return 0;
}
