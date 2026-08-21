#include <cmath>
#include <cstdint>
#include <iostream>

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

    std::cout << "test_metric_semantics: PASSED\n";
    return 0;
}
