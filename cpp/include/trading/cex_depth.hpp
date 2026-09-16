// Finite-depth CEX quotes in coin0 per coin1.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace arb::events { template <typename T> class CexDepthCursor; }

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
T add_depth_level_cash(T total, const std::vector<DepthLevel>& levels, size_t i,
                       T consumed, T fill, bool linear) {
    // Keep accumulation in the same expression as the product: separating
    // them can change fused multiply-add rounding and later trade decisions.
    if (linear && i > 0) {
        const T start = static_cast<T>(levels[i-1].price);
        const T slope = (static_cast<T>(levels[i].price)-start) / static_cast<T>(levels[i].quantity);
        return total + fill * (start + slope * (consumed + fill / T(2)));
    }
    return total + fill * static_cast<T>(levels[i].price);
}

template <typename T>
struct DepthSideCache {
    T capacity{};
    std::vector<T> cash_before;

    DepthSideCache(const std::vector<DepthLevel>& levels, bool linear) {
        cash_before.reserve(levels.size());
        T cash{};
        for (size_t i = 0; i < levels.size(); ++i) {
            cash_before.push_back(cash);
            const T quantity = static_cast<T>(levels[i].quantity);
            capacity += quantity;
            if (quantity > T(0)) cash = add_depth_level_cash(cash, levels, i, T(0), quantity, linear);
        }
    }
};

// Built once with the immutable tape, shared by every candidate. Keep each
// arithmetic target's accumulation order rather than downcasting LD quotes.
struct CexDepthQuoteCache {
    DepthSideCache<double> bids_f64, asks_f64;
    DepthSideCache<long double> bids_ld, asks_ld;

    explicit CexDepthQuoteCache(const CexDepthSnapshot& s)
        : bids_f64(s.bids, s.interpolation == DepthInterpolation::Linear),
          asks_f64(s.asks, s.interpolation == DepthInterpolation::Linear),
          bids_ld(s.bids, s.interpolation == DepthInterpolation::Linear),
          asks_ld(s.asks, s.interpolation == DepthInterpolation::Linear) {}
};

template <typename T>
class CexDepthBook {
    static_assert(std::is_floating_point_v<T>, "CexDepthBook requires floating-point arithmetic");

public:
    explicit CexDepthBook(const CexDepthSnapshot* snapshot = nullptr) {
        reset(snapshot);
    }

    void reset(const CexDepthSnapshot* snapshot) {
        // Public, possibly untrusted input still validates and prepares quotes.
        if (snapshot != nullptr) validate_cex_depth_snapshot(*snapshot);
        owned_cache_ = snapshot ? std::make_shared<CexDepthQuoteCache>(*snapshot) : nullptr;
        cache_ = owned_cache_.get();
        snapshot_ = snapshot;
        consumed_bid_ = T(0);
        consumed_ask_ = T(0);
    }

    std::optional<double> bid_price() const { return best_price(snapshot_ ? snapshot_->bids : empty_, consumed_bid_, linear()); }
    std::optional<double> ask_price() const { return best_price(snapshot_ ? snapshot_->asks : empty_, consumed_ask_, linear()); }

    std::optional<T> bid_capacity() const {
        return capacity(snapshot_ ? snapshot_->bids : empty_, consumed_bid_, side_cache(true));
    }
    std::optional<T> ask_capacity() const {
        return capacity(snapshot_ ? snapshot_->asks : empty_, consumed_ask_, side_cache(false));
    }

    std::optional<T> sell_base(T quantity) const {
        return quote(snapshot_ ? snapshot_->bids : empty_, consumed_bid_, quantity, linear(), side_cache(true));
    }
    std::optional<T> buy_base(T quantity) const {
        return quote(snapshot_ ? snapshot_->asks : empty_, consumed_ask_, quantity, linear(), side_cache(false));
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
    friend class arb::events::CexDepthCursor<T>;

    // Only the cursor of a validated immutable tape may bypass validation.
    void reset_validated(const CexDepthSnapshot* snapshot, const CexDepthQuoteCache* cache) {
        owned_cache_.reset();
        snapshot_ = snapshot;
        cache_ = cache;
        consumed_bid_ = consumed_ask_ = T(0);
    }

    const DepthSideCache<T>* side_cache(bool bid) const {
        if (!cache_) return nullptr;
        if constexpr (std::is_same_v<T, double>)
            return bid ? &cache_->bids_f64 : &cache_->asks_f64;
        else if constexpr (std::is_same_v<T, long double>)
            return bid ? &cache_->bids_ld : &cache_->asks_ld;
        else return nullptr;
    }

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

    static std::optional<T> capacity(const std::vector<DepthLevel>& levels, T consumed,
                                     const DepthSideCache<T>* cached) {
        if (cached && consumed == T(0)) return cached->capacity;
        return remaining_capacity(levels, consumed);
    }

    static std::optional<T> quote(
        const std::vector<DepthLevel>& levels, T consumed, T quantity, bool linear,
        const DepthSideCache<T>* cached) {
        if (levels.empty() || !valid_quantity(quantity)) return std::nullopt;
        if (quantity == T(0)) return T(0);
        const auto available_capacity = capacity(levels, consumed, cached);
        if (!available_capacity || quantity > *available_capacity) return std::nullopt;

        if (cached && consumed == T(0)) {
            // Preserve the original repeated subtraction and final partial-fill
            // arithmetic. Prefix cash skips recomputing every full level's cost.
            T remaining = quantity;
            for (size_t i = 0; i < levels.size(); ++i) {
                const T level_quantity = static_cast<T>(levels[i].quantity);
                if (level_quantity <= T(0)) continue;
                if (remaining <= level_quantity)
                    return add_depth_level_cash(cached->cash_before[i], levels, i, T(0), remaining, linear);
                remaining -= level_quantity;
            }
            return std::nullopt;
        }

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
            total = add_depth_level_cash(total, levels, i, consumed, fill, linear);
            if (fill >= remaining) return total;
            remaining -= fill;
            consumed = T(0);
        }
        return std::nullopt;
    }

    const CexDepthSnapshot* snapshot_{nullptr};
    const CexDepthQuoteCache* cache_{nullptr};
    std::shared_ptr<const CexDepthQuoteCache> owned_cache_;
    T consumed_bid_{T(0)};
    T consumed_ask_{T(0)};
};

} // namespace arb::trading
