// Observer/metric accumulator precision policy.
#pragma once

#include <type_traits>

namespace arb {
namespace harness {

template <typename T>
using MetricF =
    std::conditional_t<std::is_same_v<T, long double>, long double, double>;

} // namespace harness
} // namespace arb
