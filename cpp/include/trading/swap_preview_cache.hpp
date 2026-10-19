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
    // The report-independent part of one sizing (decide_trade): the size ladder and the floor-fee
    // envelope it was capped with depend only on the pool, the event price, the direction, the
    // spot fee, the global floor fee and the fixed costs, so every report context of the same
    // event reuses it.
    struct Ladder {
        static constexpr int MAX = 24;
        bool valid{false};
        int sel_i{-1};
        T fee_pool{}, floor_fee{}, gas{}, cex_price{}, p_now{}, dx_lo{}, dx_hi{};
        int m_pts{0};
        std::array<T, MAX> pts;
        bool cap_is_hi{false};
        T cap_envelope{};  // floor-fee envelope value at the last rung when it is the envelope cap
        T dx_cap{}, dx_dip{}, low_cover{};
        bool matches(int i, const T& f0, const T& ff, const T& g, const T& p, const T& pn, const T& lo, const T& hi) const {
            return valid && sel_i == i && fee_pool == f0 && floor_fee == ff && gas == g && cex_price == p &&
                   p_now == pn && dx_lo == lo && dx_hi == hi;
        }
    };
    Ladder ladder;

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
