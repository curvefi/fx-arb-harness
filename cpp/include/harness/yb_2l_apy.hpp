// APY accounting derived from the state-mutating YieldBasis actor.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "harness/precision.hpp"

namespace arb::harness {

template <typename T>
class Yb2LApyTracker {
public:
    using F = MetricF<T>;
    static constexpr F SECONDS_PER_YEAR = F(365.0 * 86400.0);
    static constexpr F MAX_ANNUALIZED_LOG = F(700.0);
    static constexpr F NEG_DISC_EPS = F(-1e-14L);

    bool initialized() const { return initialized_; }

    template <typename Pool, typename Actor>
    void sample(const Pool& pool, const Actor& actor, uint64_t ts) {
        const F oracle = static_cast<F>(actor.lp_oracle(pool));
        const F collateral = static_cast<F>(actor.state().collateral);
        const F debt = static_cast<F>(actor.projected_debt(ts));
        const F lev_ratio = static_cast<F>(actor.state().lev_ratio);
        const F leverage = static_cast<F>(actor.state().leverage);
        const F price_scale = static_cast<F>(pool.cached_price_scale);
        if (!(oracle > F(0)) || !(collateral > F(0)) || !(lev_ratio > F(0)) ||
            !(leverage > F(0)) || !(price_scale > F(0))) {
            return;
        }
        const F value = marked_value(oracle, collateral, debt, lev_ratio);
        if (!initialized_) {
            if (!(value > F(0))) return;
            initialized_ = true;
            first_t_ = ts;
            last_t_ = ts;
            initial_value_ = value;
            initial_price_scale_ = price_scale;
            last_growth_ = F(1);
            return;
        }
        const F price_factor = std::pow(
            price_scale / initial_price_scale_, leverage / F(2)
        );
        last_growth_ = value > F(0) && price_factor > F(0)
            ? (value / initial_value_) / price_factor
            : F(0);
        last_t_ = ts;
    }

    F growth() const { return initialized_ ? last_growth_ : F(-1); }

    double final_growth() const {
        return initialized_ ? static_cast<double>(last_growth_) : -1.0;
    }

    double apy() const {
        if (!initialized_ || !(last_t_ > first_t_) || !(last_growth_ > F(0))) {
            return -1.0;
        }
        const F duration_s = static_cast<F>(last_t_ - first_t_);
        const F annualized_log =
            std::log(last_growth_) * SECONDS_PER_YEAR / duration_s;
        if (annualized_log > MAX_ANNUALIZED_LOG) {
            return static_cast<double>(std::exp(MAX_ANNUALIZED_LOG));
        }
        return static_cast<double>(std::expm1(annualized_log));
    }

private:
    bool initialized_{false};
    uint64_t first_t_{0};
    uint64_t last_t_{0};
    F initial_value_{F(0)};
    F initial_price_scale_{F(0)};
    F last_growth_{F(-1)};

    static F marked_value(F oracle, F collateral, F debt, F lev_ratio) {
        const F coll_value = oracle * collateral;
        F disc = coll_value * coll_value
            - F(4) * coll_value * lev_ratio * debt;
        if (disc < F(0)) {
            if (disc > NEG_DISC_EPS) {
                disc = F(0);
            } else {
                return F(0);
            }
        }
        const F x0 = (coll_value + std::sqrt(disc)) / (F(2) * lev_ratio);
        const F invariant_product =
            (x0 - debt) * collateral * oracle;
        return F(2) * std::sqrt(std::max(invariant_product, F(0))) - x0;
    }
};

} // namespace arb::harness
