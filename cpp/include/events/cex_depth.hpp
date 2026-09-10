// Causal finite-depth market-data tape and per-run consumption cursor.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "trading/cex_depth.hpp"

namespace arb::events {

class CexDepthTape {
public:
    explicit CexDepthTape(std::vector<trading::CexDepthSnapshot> snapshots);

    const std::vector<trading::CexDepthSnapshot>& snapshots() const {
        return snapshots_;
    }

private:
    std::vector<trading::CexDepthSnapshot> snapshots_;
};

CexDepthTape load_cex_depth(const std::string& path);

template <typename T>
class CexDepthCursor {
public:
    CexDepthCursor(const CexDepthTape& tape, uint64_t max_age_s)
        : tape_(tape), max_age_s_(max_age_s) {}

    // Returns the current book midpoint, or nullopt when all published data is
    // future or stale. Consumption survives until a new snapshot is published.
    std::optional<T> advance(uint64_t event_ts_s) {
        constexpr uint64_t NS_PER_S = 1'000'000'000ULL;
        if (event_ts_s > UINT64_MAX / NS_PER_S) {
            throw std::overflow_error("event timestamp cannot be represented in nanoseconds");
        }
        return advance_ns(event_ts_s * NS_PER_S);
    }

    std::optional<T> advance_ns(uint64_t wall_ns) {
        constexpr uint64_t NS_PER_S = 1'000'000'000ULL;
        const auto& snapshots = tape_.snapshots();
        while (next_ < snapshots.size() && snapshots[next_].available_ns <= wall_ns) {
            current_ = &snapshots[next_++];
            book_.reset(current_);
        }
        if (current_ == nullptr) return std::nullopt;

        const uint64_t age_ns = wall_ns - current_->available_ns;
        const uint64_t age_seconds = age_ns / NS_PER_S;
        const bool within_age = age_seconds < max_age_s_ ||
            (age_seconds == max_age_s_ && age_ns % NS_PER_S == 0);
        if (!within_age) return std::nullopt;

        const double bid = current_->bids.front().price;
        const double ask = current_->asks.front().price;
        return static_cast<T>(bid + (ask - bid) / 2.0);
    }

    trading::CexDepthBook<T>& book() { return book_; }
    std::optional<uint64_t> current_available_ns() const {
        return current_ == nullptr ? std::nullopt
                                   : std::optional<uint64_t>(current_->available_ns);
    }

private:
    const CexDepthTape& tape_;
    uint64_t max_age_s_;
    size_t next_{0};
    const trading::CexDepthSnapshot* current_{nullptr};
    trading::CexDepthBook<T> book_;
};

} // namespace arb::events
