// Finite-depth CEX quotes in coin0 per coin1.
#pragma once

#include <algorithm>
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

enum class DepthInterpolation { Step, Linear };

// The snapshot must remain immutable for the lifetime of any book viewing it.
// Snapshot selection and replacement belong to the future market-data loader.
struct CexDepthSnapshot {
    uint64_t available_ns{};
    std::vector<DepthLevel> bids; // descending price
    std::vector<DepthLevel> asks; // ascending price
    DepthInterpolation interpolation{DepthInterpolation::Step};
};

inline void validate_cex_depth_snapshot(const CexDepthSnapshot& snapshot) {
    if (snapshot.available_ns == 0 || snapshot.bids.empty() || snapshot.asks.empty()) {
        throw std::invalid_argument("CEX depth snapshot is missing required data");
    }
    const auto valid_level = [&](const DepthLevel& level) {
        return std::isfinite(level.price) && level.price > 0.0 &&
            std::isfinite(level.quantity) && (level.quantity > 0.0 ||
                (snapshot.interpolation == DepthInterpolation::Linear && level.quantity == 0.0));
    };
    for (size_t i = 0; i < snapshot.bids.size(); ++i) {
        if (!valid_level(snapshot.bids[i]) ||
            (i != 0 && snapshot.bids[i - 1].price < snapshot.bids[i].price)) {
            throw std::invalid_argument("invalid or unsorted CEX bid depth");
        }
    }
    for (size_t i = 0; i < snapshot.asks.size(); ++i) {
        if (!valid_level(snapshot.asks[i]) ||
            (i != 0 && snapshot.asks[i - 1].price > snapshot.asks[i].price)) {
            throw std::invalid_argument("invalid or unsorted CEX ask depth");
        }
    }
    if (snapshot.bids.front().price > snapshot.asks.front().price) {
        throw std::invalid_argument("crossed CEX depth snapshot");
    }
    if (snapshot.bids.front().quantity <= 0 || snapshot.asks.front().quantity <= 0)
        throw std::invalid_argument("best CEX level must have positive quantity");
}

template <typename T>
class CexDepthBook {
    static_assert(std::is_floating_point_v<T>, "CexDepthBook requires floating-point arithmetic");

public:
    explicit CexDepthBook(const CexDepthSnapshot* snapshot = nullptr) {
        reset(snapshot);
    }

    void reset(const CexDepthSnapshot* snapshot) {
        if (snapshot != nullptr) validate_cex_depth_snapshot(*snapshot);
        snapshot_ = snapshot;
        consumed_bid_ = T(0);
        consumed_ask_ = T(0);
    }

    std::optional<double> bid_price() const { return best_price(snapshot_ ? snapshot_->bids : empty_, consumed_bid_, linear()); }
    std::optional<double> ask_price() const { return best_price(snapshot_ ? snapshot_->asks : empty_, consumed_ask_, linear()); }

    std::optional<T> bid_capacity() const {
        return remaining_capacity(snapshot_ ? snapshot_->bids : empty_, consumed_bid_);
    }
    std::optional<T> ask_capacity() const {
        return remaining_capacity(snapshot_ ? snapshot_->asks : empty_, consumed_ask_);
    }

    std::optional<T> sell_base(T quantity) const {
        return quote(snapshot_ ? snapshot_->bids : empty_, consumed_bid_, quantity, linear());
    }
    std::optional<T> buy_base(T quantity) const {
        return quote(snapshot_ ? snapshot_->asks : empty_, consumed_ask_, quantity, linear());
    }

    bool consume_sell(T quantity) {
        const auto quoted = sell_base(quantity);
        if (!quoted) return false;
        consumed_bid_ += quantity;
        return true;
    }

    bool consume_buy(T quantity) {
        const auto quoted = buy_base(quantity);
        if (!quoted) return false;
        consumed_ask_ += quantity;
        return true;
    }

private:
    bool linear() const { return snapshot_ && snapshot_->interpolation == DepthInterpolation::Linear; }
    inline static const std::vector<DepthLevel> empty_{};

    static bool valid_quantity(T quantity) {
        return std::isfinite(static_cast<long double>(quantity)) && quantity >= T(0);
    }

    static std::optional<double> best_price(
        const std::vector<DepthLevel>& levels, T consumed, bool linear) {
        if (levels.empty()) return std::nullopt;
        for (size_t i = 0; i < levels.size(); ++i) {
            const auto& level = levels[i];
            const T quantity = static_cast<T>(level.quantity);
            if (consumed < quantity) {
                if (!linear || i == 0) return level.price;
                return levels[i-1].price + (level.price-levels[i-1].price)*static_cast<double>(consumed/quantity);
            }
            consumed -= quantity;
        }
        return std::nullopt;
    }

    static std::optional<T> remaining_capacity(
        const std::vector<DepthLevel>& levels, T consumed) {
        if (levels.empty()) return std::nullopt;
        T capacity = T(0);
        for (const auto& level : levels) {
            const T quantity = static_cast<T>(level.quantity);
            if (consumed >= quantity) {
                consumed -= quantity;
            } else {
                capacity += quantity - consumed;
                consumed = T(0);
            }
        }
        return capacity > T(0) ? std::optional<T>(capacity) : std::nullopt;
    }

    static std::optional<T> quote(
        const std::vector<DepthLevel>& levels, T consumed, T quantity, bool linear) {
        if (levels.empty() || !valid_quantity(quantity)) return std::nullopt;
        if (quantity == T(0)) return T(0);
        const auto capacity = remaining_capacity(levels, consumed);
        if (!capacity || quantity > *capacity) return std::nullopt;

        T total = T(0);
        T remaining = quantity;
        for (size_t i = 0; i < levels.size(); ++i) {
            const auto& level = levels[i];
            const T level_quantity = static_cast<T>(level.quantity);
            if (consumed >= level_quantity) {
                consumed -= level_quantity;
                continue;
            }
            const T available = level_quantity - consumed;
            const T fill = std::min(available, remaining);
            if (linear && i > 0) {
                const T start = static_cast<T>(levels[i-1].price);
                const T slope = (static_cast<T>(level.price)-start)/level_quantity;
                total += fill*(start+slope*(consumed+fill/T(2)));
            } else {
                total += fill * static_cast<T>(level.price);
            }
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
