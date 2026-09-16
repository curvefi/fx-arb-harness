#include "events/cex_depth.hpp"

#include <utility>

namespace arb::events {

CexDepthTape::CexDepthTape(std::vector<trading::CexDepthSnapshot> snapshots)
    : snapshots_(std::move(snapshots)) {
    if (snapshots_.empty()) {
        throw std::invalid_argument("CEX depth tape must not be empty");
    }
    uint64_t previous_ns = 0;
    quote_cache_.reserve(snapshots_.size());
    for (const auto& snapshot : snapshots_) {
        trading::validate_cex_depth_snapshot(snapshot);
        if (snapshot.available_ns <= previous_ns) {
            throw std::invalid_argument(
                "CEX depth available_ns timestamps must be strictly increasing"
            );
        }
        previous_ns = snapshot.available_ns;
        quote_cache_.emplace_back(snapshot);
    }
}

} // namespace arb::events
