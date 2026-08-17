#pragma once

#include <stdexcept>

namespace arb {
namespace pools {
namespace twocrypto_fx {

template <typename T>
struct ChallengeFeePolicy {
    struct State {
        unsigned int update_count{0};
        bool throw_on_update{false};
    };

    static T get_fee(
        const State&,
        const PolicyConfig<T>&,
        const PolicyPoolConfig<T>&,
        const PolicyResearchContext<T>&,
        const std::array<T, 2>&
    ) {
        return T(0);
    }

    static T get_price_scale(
        State&,
        PolicyResearchContext<T>&,
        const PolicyConfig<T>&,
        const PolicyPoolConfig<T>&
    ) {
        return T(0);
    }

    static void update_state(
        State& state,
        PolicyResearchContext<T>&,
        const PolicyConfig<T>&,
        const PolicyPoolConfig<T>&,
        const PolicyUpdate<T>&
    ) {
        ++state.update_count;
        if (state.throw_on_update) {
            throw std::runtime_error("blocked policy update");
        }
    }

    static T fee_floor(
        const PolicyConfig<T>&,
        const PolicyPoolConfig<T>&,
        const T& native_floor
    ) {
        return native_floor;
    }
};

} // namespace twocrypto_fx
} // namespace pools
} // namespace arb
