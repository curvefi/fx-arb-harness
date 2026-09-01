#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

#include "harness/metrics.hpp"

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
    constexpr uint64_t day = 24ULL * 60ULL * 60ULL;
    using RobustApy = arb::harness::NetApyRobust90d<double>;
    RobustApy metric;
    metric.reserve_duration(200 * day);
    std::vector<double> net_growth{1.0};
    for (uint64_t i = 1; i <= 200; ++i) {
        const double daily_log_growth =
            i >= 105 && i < 145 ? -0.001 : 0.0004;
        net_growth.push_back(net_growth.back() * std::exp(daily_log_growth));
    }
    for (uint64_t i = 0; i <= 200; ++i) {
        metric.sample(i * day, net_growth[i]);
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
        near(metric.value(), std::expm1((mean_rate + tail_rate) / 2.0)),
        "mixed robust APY mismatch"
    );

    std::cout << "test_metric_semantics: PASSED\n";
    return 0;
}
