#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <vector>

#include "harness/metrics.hpp"
#include "harness/yb_2l_apy.hpp"

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

struct MarkPool { double cached_price_scale{1}; };
struct MarkActor {
    struct State {
        double collateral{1}, debt{0.5}, lev_ratio{4.0/9}, leverage{2}, stable_aggregator{1};
    } values;
    const State& state() const { return values; }
    double lp_oracle(const MarkPool& pool) const {
        return std::sqrt(pool.cached_price_scale) * values.stable_aggregator;
    }
    double projected_debt(uint64_t) const { return values.debt; }
};

void par_mark_matches_book_at_one() {
    MarkPool pool;
    MarkActor actor;
    arb::harness::Yb2LApyTracker<double> book, par;
    arb::harness::RollingGeoApy<double> book_gm, par_gm;
    for (uint64_t hour = 0; hour <= 96*24; ++hour) {
        actor.values.collateral = std::exp(double(hour) * 1e-5);
        book.sample(pool, actor, hour*3600);
        par.sample_par(pool, actor, hour*3600);
        require(par.valid() && par.growth() == book.growth(), "par mark must equal book when rate is one");
        book_gm.sample(hour*3600, book.growth());
        par_gm.sample(hour*3600, par.growth());
    }
    require(par.apy() == book.apy() && par.final_growth() == book.final_growth(), "par endpoints differ at rate one");
    const auto a = book_gm.summary(), b = par_gm.summary();
    require(a.gm30 == b.gm30 && a.gm60 == b.gm60 && a.gm90 == b.gm90 &&
            a.floor_share30 == b.floor_share30 && a.floor_share60 == b.floor_share60 && a.floor_share90 == b.floor_share90 &&
            a.worst30 == b.worst30 && a.worst60 == b.worst60 && a.worst90 == b.worst90 && a.worst60_end == b.worst60_end,
            "par rolling family differs at rate one");
}

void par_mark_removes_only_the_mark_and_latches_invalidity() {
    MarkPool pool;
    MarkActor actor;
    arb::harness::Yb2LApyTracker<double> par;
    uint64_t ts = 0;
    // Initial rate need not be one; changing only the mark cannot create returns.
    for (double rate : {0.98, 0.90, 1.02, 1.0}) {
        actor.values.stable_aggregator = rate;
        par.sample_par(pool, actor, ts);
        require(par.valid() && par.growth() == 1, "par normalization or fixed-state rate neutrality failed");
        require(actor.values.stable_aggregator == rate, "par sampler mutated the actor rate");
        ts += 3600;
    }
    const auto healthy = par;
    actor.values.collateral = 0.99;
    par.sample_par(pool, actor, ts);
    require(near(par.growth(), 0.979789380293346), "par mark hid a collateral loss");
    auto debt_loss = healthy;
    actor.values.collateral = 1;
    actor.values.debt = 0.505;
    debt_loss.sample_par(pool, actor, ts);
    require(near(debt_loss.growth(), 0.9897915761656364), "par mark hid accrued debt");
    for (int failure = 0; failure < 3; ++failure) {
        actor.values.debt = failure == 2 ? 1.0 : 0.5;
        actor.values.stable_aggregator = failure == 0 ? 0.0 : failure == 1 ? std::numeric_limits<double>::quiet_NaN() : 1.0;
        auto invalid = healthy;
        invalid.sample_par(pool, actor, ts);
        require(invalid.invalid() && !invalid.valid(), "invalid par value was accepted");
        actor.values.debt = 0.5;
        actor.values.stable_aggregator = 1;
        invalid.sample_par(pool, actor, ts+3600);
        require(invalid.invalid() && invalid.growth() == -1 && invalid.final_growth() == -1 && invalid.apy() == -1,
                "par metric recovered by silently discarding an invalid sample");
    }
}

} // namespace

int main() {
    par_mark_matches_book_at_one();
    par_mark_removes_only_the_mark_and_latches_invalidity();
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
    // intrahour samples changing the hourly stream or the three-window family.
    using Window = arb::harness::RollingGeoApyWindow<double>;
    Window horizons[] = {{30 * day, 1e-20}, {60 * day, 1e-20}, {90 * day, 1e-20}};
    arb::harness::RollingGeoApy<double> family;
    std::vector<double> hourly_growth;
    for (uint64_t hour = 0; hour <= 120 * 24; ++hour) {
        const double growth = std::exp(0.0004 * std::min(double(hour) / 24, 50.0));
        hourly_growth.push_back(growth);
        family.sample(hour * 3600, growth);
        for (auto& horizon : horizons) {
            horizon.sample(hour * 3600, growth);
            horizon.sample(hour * 3600 + 1, growth * 2); // Not due.
            if (hour * 3600 < horizon.WINDOW_S)
                require(horizon.value() == -1, "GM must wait for a full horizon");
        }
    }
    for (auto& horizon : horizons) {
        const size_t hours = horizon.WINDOW_S / 3600;
        double sum_log = 0;
        size_t floored = 0;
        for (size_t end = hours; end < hourly_growth.size(); ++end) {
            const double apy = std::pow(hourly_growth[end] / hourly_growth[end-hours],
                                        365.0 * 24 / hours) - 1;
            sum_log += std::log(std::max(apy, 1e-20));
            floored += apy < 1e-20;
        }
        const size_t count = hourly_growth.size() - hours;
        require(near(horizon.value(), std::exp(sum_log / count)),
                "rolling GM must match the batch oracle for every horizon");
        require(horizon.n_windows == count && horizon.n_floored_windows == floored,
                "GM window and flat-tail floor counts must match");
        require(near(horizon.floor_share(), double(floored) / count), "GM floor share mismatch");
        require(horizon.samples.size() <= hours + 1, "GM storage must be bounded");
    }
    require(horizons[0].value() < horizons[1].value() &&
            horizons[1].value() < horizons[2].value(),
            "shorter horizons must expose the flat tail sooner");
    const auto gm = family.summary();
    require(gm.gm30 == horizons[0].value() && gm.gm60 == horizons[1].value() && gm.gm90 == horizons[2].value() &&
            gm.floor_share30 == horizons[0].floor_share() && gm.floor_share90 == horizons[2].floor_share(),
            "the family must equal its three windows");

    arb::harness::SampledLogVariation<double> variation;
    require(variation.annualized() == -1, "QV needs a positive duration");
    variation.sample(0, 100); variation.sample(3600, 110); variation.sample(7200, 100);
    require(near(variation.annualized(), std::pow(std::log(1.1), 2) * 365 * 24),
            "annualized hourly QV counts both directions without cancellation");
    std::cout << "test_metric_semantics: PASSED\n";
    return 0;
}
