// Common utilities: differs_rel.
#pragma once

#include <cmath>
#include <algorithm>

namespace arb {

// Relative difference check for floating-point comparison
// Returns true if values differ by more than 1e-12 * max(1, max(|a|, |b|))
template <typename T>
inline bool differs_rel(T a, T b) {
    const T da    = std::abs(a - b);
    const T scale = std::max<T>(T(1), std::max(std::abs(a), std::abs(b)));
    return da > T(1e-12) * scale;
}

} // namespace arb
