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
    // Coin0 cash and coin1 token units, with a coin0/coin1 external mark.
    // Initial inventory is 100 cash + 2 tokens; at price 100, hold is 300.
    using arb::harness::pool_nav_vs_hold;
    require(near(pool_nav_vs_hold<double>({100,2}, {100,2}, 200), 0),
            "unchanged token inventory must match hold after a price move");
    require(near(pool_nav_vs_hold<double>({100,2}, {80,2.4}, 100), 1.0/15),
            "320 ending NAV must outperform 300 held NAV by 6.6667 percent");
    require(near(pool_nav_vs_hold<double>({100,2}, {110,2}, 100), 1.0/30),
            "gross NAV must include a 10-coin0 donation");
    require(near(pool_nav_vs_hold<double>({100,2}, {200,4}, 100), 1),
            "total-assets metric must include new capital rather than normalize LP supply");

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

    // Independent batch oracle: profitable first 50 days, then a flat tail.
    // The shorter horizons must see more zero-return windows, without extra
    // intrahour samples changing the hourly stream or the legacy 90-day value.
    using Window = arb::harness::RollingGeoApyWindow<double>;
    Window horizons[] = {{30 * day, 1e-20}, {60 * day, 1e-20}, {90 * day, 1e-20}};
    arb::harness::RollingGeoApy90d<double> legacy;
    std::vector<double> hourly_growth;
    for (uint64_t hour = 0; hour <= 120 * 24; ++hour) {
        const double growth = std::exp(0.0004 * std::min(double(hour) / 24, 50.0));
        hourly_growth.push_back(growth);
        legacy.sample(hour * 3600, growth);
        for (auto& horizon : horizons) {
            horizon.sample(hour * 3600, growth);
            horizon.sample(hour * 3600 + 1, growth * 2); // Not due.
            if (hour * 3600 < horizon.WINDOW_S)
                require(horizon.value() == -1, "GM must wait for a full horizon");
        }
    }
    for (auto& horizon : horizons) {
        const size_t hours = horizon.WINDOW_S / 3600;
        double sum_log = 0, sum_positive_log = 0;
        size_t floored = 0;
        for (size_t end = hours; end < hourly_growth.size(); ++end) {
            const double apy = std::pow(hourly_growth[end] / hourly_growth[end-hours],
                                        365.0 * 24 / hours) - 1;
            sum_log += std::log(std::max(apy, 1e-20));
            floored += apy < 1e-20;
            if (apy >= 1e-20) sum_positive_log += std::log(apy);
        }
        const size_t count = hourly_growth.size() - hours;
        require(near(horizon.value(), std::exp(sum_log / count)),
                "rolling GM must match the batch oracle for every horizon");
        require(horizon.n_windows == count && horizon.n_floored_windows == floored,
                "GM window and flat-tail floor counts must match");
        require(near(horizon.floor_share(), double(floored) / count), "GM floor share mismatch");
        require(near(horizon.unfloored_value(), std::exp(sum_positive_log / (count - floored))),
                "unfloored GM diagnostic must match the positive-window oracle");
        require(horizon.samples.size() <= hours + 1, "GM storage must be bounded");
    }
    require(horizons[0].value() < horizons[1].value() &&
            horizons[1].value() < horizons[2].value(),
            "shorter horizons must expose the flat tail sooner");
    require(horizons[2].value() == legacy.value(), "90-day legacy alias must be exact");

    arb::harness::SampledLogVariation<double> variation;
    require(variation.annualized() == -1, "QV needs a positive duration");
    variation.sample(0, 100); variation.sample(3600, 110); variation.sample(7200, 100);
    require(near(variation.annualized(), std::pow(std::log(1.1), 2) * 365 * 24),
            "annualized hourly QV counts both directions without cancellation");
    std::cout << "test_metric_semantics: PASSED\n";
    return 0;
}
