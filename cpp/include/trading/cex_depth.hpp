// Executable CEX levels in coin0 per coin1: a maker's trade-through fill book,
// or an explicit book passed to a YB hedge.
#pragma once

#include <cmath>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace arb::trading {

struct DepthLevel {
    double price{};
    double quantity{}; // always coin1
};

// Bids descending (where base can be sold), asks ascending (where base can be
// bought). A fill book lists one bin's prints: either side may be empty and
// prints of a moving market may cross.
struct CexDepthSnapshot {
    uint64_t available_ns{};
    std::vector<DepthLevel> bids;
    std::vector<DepthLevel> asks;
};

inline void validate_cex_depth_snapshot(const CexDepthSnapshot& snapshot) {
    const auto side = [](const std::vector<DepthLevel>& levels, bool descending) {
        for (size_t i = 0; i < levels.size(); ++i) {
            const auto& level = levels[i];
            if (!std::isfinite(level.price) || !(level.price > 0.0) ||
                !std::isfinite(level.quantity) || !(level.quantity > 0.0) ||
                (i != 0 && (descending ? levels[i - 1].price < level.price
                                       : levels[i - 1].price > level.price)))
                throw std::invalid_argument("invalid or unsorted CEX levels");
        }
    };
    side(snapshot.bids, true);
    side(snapshot.asks, false);
}

template <typename T>
class CexDepthBook {
    static_assert(std::is_floating_point_v<T>, "CexDepthBook requires floating-point arithmetic");

public:
    explicit CexDepthBook(const CexDepthSnapshot* snapshot = nullptr) { reset(snapshot); }

    void reset(const CexDepthSnapshot* snapshot) {
        if (snapshot != nullptr) validate_cex_depth_snapshot(*snapshot);
        snapshot_ = snapshot;
        consumed_bid_ = T(0);
        consumed_ask_ = T(0);
    }

    std::optional<double> bid_price() const { return best_price(bids(), consumed_bid_); }
    std::optional<double> ask_price() const { return best_price(asks(), consumed_ask_); }
    std::optional<T> bid_capacity() const { return capacity(bids(), consumed_bid_); }
    std::optional<T> ask_capacity() const { return capacity(asks(), consumed_ask_); }
    std::optional<T> sell_base(T quantity) const { return quote(bids(), consumed_bid_, quantity); }
    std::optional<T> buy_base(T quantity) const { return quote(asks(), consumed_ask_, quantity); }

    bool consume_sell(T quantity) {
        if (!sell_base(quantity)) return false;
        consumed_bid_ += quantity;
        return true;
    }

    bool consume_buy(T quantity) {
        if (!buy_base(quantity)) return false;
        consumed_ask_ += quantity;
        return true;
    }

private:
    inline static const std::vector<DepthLevel> empty_{};
    const std::vector<DepthLevel>& bids() const { return snapshot_ ? snapshot_->bids : empty_; }
    const std::vector<DepthLevel>& asks() const { return snapshot_ ? snapshot_->asks : empty_; }

    static std::optional<double> best_price(const std::vector<DepthLevel>& levels, T consumed) {
        for (const auto& level : levels) {
            const T quantity = static_cast<T>(level.quantity);
            if (consumed < quantity) return level.price;
            consumed -= quantity;
        }
        return std::nullopt;
    }

    static std::optional<T> capacity(const std::vector<DepthLevel>& levels, T consumed) {
        T total = T(0);
        for (const auto& level : levels) {
            const T quantity = static_cast<T>(level.quantity);
            if (consumed >= quantity) {
                consumed -= quantity;
            } else {
                total += quantity - consumed;
                consumed = T(0);
            }
        }
        return total > T(0) ? std::optional<T>(total) : std::nullopt;
    }

    static std::optional<T> quote(const std::vector<DepthLevel>& levels, T consumed, T quantity) {
        if (!std::isfinite(static_cast<long double>(quantity)) || quantity < T(0)) return std::nullopt;
        if (quantity == T(0)) return T(0);
        const auto available = capacity(levels, consumed);
        if (!available || quantity > *available) return std::nullopt;
        T total = T(0);
        T remaining = quantity;
        for (const auto& level : levels) {
            const T level_quantity = static_cast<T>(level.quantity);
            if (consumed >= level_quantity) {
                consumed -= level_quantity;
                continue;
            }
            const T fill = std::min(level_quantity - consumed, remaining);
            total += fill * static_cast<T>(level.price);
            if (fill >= remaining) return total;
            remaining -= fill;
            consumed = T(0);
        }
        return std::nullopt;
    }

    const CexDepthSnapshot* snapshot_{nullptr};
    T consumed_bid_{T(0)};
    T consumed_ask_{T(0)};
};

} // namespace arb::trading
