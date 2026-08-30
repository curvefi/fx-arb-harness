#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

#include "harness/metrics.hpp"
#include "harness/runner.hpp"

namespace {

bool near(double actual, double expected) {
    return std::abs(actual - expected) < 1e-12;
}

void require(bool condition, const char* msg) {
    if (!condition) {
        std::cerr << "FAIL: " << msg << std::endl;
        std::exit(1);
    }
}

} // namespace

int main() {
    using Slippage = arb::harness::SlippageProbes<double>;
    static_assert(
        Slippage::N_SIZES == 3 && Slippage::SIZE_FRACS[0] == 0.01 &&
        Slippage::SIZE_FRACS[1] == 0.05 && Slippage::SIZE_FRACS[2] == 0.10);

    Slippage probes{};
    probes.sample(2, 10, 0.03, 0.06);
    probes.accumulate_previous(2, 20);
    require(near(probes.tw_slippage(2), 0.045), "10% slippage probe mismatch");
    require(near(arb::harness::tvl_growth(100.0, 125.0), 1.25), "tvl_growth ratio mismatch");

    arb::harness::MultiScalePositiveGrowthConcentration<double> growth;
    constexpr uint64_t day = 24ULL * 60ULL * 60ULL;

    double value = 1.0;
    growth.sample(0, value);
    for (uint64_t i = 1; i <= 11; ++i) {
        value += (i == 11) ? 10.0 : 1.0;
        growth.sample(i * day, value);
    }

    require(near(growth.block_share(1, 1), 0.50), "block_share(1, 1) mismatch");
    require(near(growth.block_share(1, 10), 0.95), "block_share(1, 10) mismatch");
    require(near(growth.block_share(3, 10), 1.00), "block_share(3, 10) mismatch");
    require(near(growth.block_share(7, 5), 1.00), "block_share(7, 5) mismatch");

    using RobustApy = arb::harness::NetApyRobust90d<double>;
    RobustApy constant_apy;
    constexpr double constant_log_rate = 0.08;
    for (uint64_t i = 0; i <= 110; ++i) {
        constant_apy.sample(
            i * day,
            std::exp(constant_log_rate * static_cast<double>(i) / 365.0)
        );
    }
    require(
        near(constant_apy.value(), std::expm1(constant_log_rate)),
        "constant robust APY mismatch"
    );

    RobustApy mixed_apy;
    mixed_apy.reserve_duration(200 * day);
    std::vector<double> net_growth{1.0};
    for (uint64_t i = 1; i <= 200; ++i) {
        const double daily_log_growth =
            i >= 105 && i < 145 ? -0.001 : 0.0004;
        net_growth.push_back(net_growth.back() * std::exp(daily_log_growth));
    }
    for (uint64_t i = 0; i <= 200; ++i) {
        mixed_apy.sample(i * day, net_growth[i]);
    }

    std::vector<double> window_rates;
    for (size_t i = 90; i < net_growth.size(); ++i) {
        window_rates.push_back(
            std::log(net_growth[i] / net_growth[i - 90]) * 365.0 / 90.0
        );
    }
    const double mean_rate = [&] {
        double sum = 0.0;
        for (double rate : window_rates) sum += rate;
        return sum / static_cast<double>(window_rates.size());
    }();
    std::sort(window_rates.begin(), window_rates.end());
    const size_t tail_count = (window_rates.size() + 19) / 20;
    double tail_rate = 0.0;
    for (size_t i = 0; i < tail_count; ++i) tail_rate += window_rates[i];
    tail_rate /= static_cast<double>(tail_count);
    require(
        near(mixed_apy.value(), std::expm1((mean_rate + tail_rate) / 2.0)),
        "mixed robust APY mismatch"
    );

    std::cout << "test_metric_semantics: PASSED\n";
    return 0;
}
