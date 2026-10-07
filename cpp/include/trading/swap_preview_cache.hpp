#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include "pools/twocrypto_fx/helpers.hpp"

namespace arb::trading {

// Scratch space for one unchanged pool state. Reports may change; fees are never
// cached. Exact keys and replacement on collision preserve every requested size.
template <typename T>
class SwapPreviewCache {
    struct Entry {
        T dx;
        std::array<T, 2> xp;
        T gross_output;
        size_t input;
    };
    std::array<Entry, 64> entries_;
    uint64_t valid_{0};
public:
    template <typename Pool, typename Fee>
    std::pair<T, T> quote(const Pool& pool, size_t i, size_t j, T dx, const Fee& fee) {
        const size_t slot = (std::hash<T>{}(dx) ^ (i * 31)) & 63;
        const uint64_t bit = uint64_t(1) << slot;
        auto& entry = entries_[slot];
        if (!(valid_ & bit) || entry.dx != dx || entry.input != i) {
            const T scale = pool.cached_price_scale;
            auto [xp, dy] = pools::twocrypto_fx::post_swap_xp(pool, i, j, dx, scale);
            entry = {dx, xp, pools::twocrypto_fx::xp_to_tokens_j(pool, j, dy, scale), i};
            valid_ |= bit;
        }
        const T fee_tokens = fee(entry.xp) * entry.gross_output /
            pools::twocrypto_fx::PoolTraits<T>::FEE_PRECISION();
        return {entry.gross_output - fee_tokens, fee_tokens};
    }
};

} // namespace arb::trading
