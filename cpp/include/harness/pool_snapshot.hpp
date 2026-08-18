// Transaction rollback through the pool SDK's authoritative mutable surface.
#pragma once

#include <type_traits>
#include <utility>

namespace arb {
namespace harness {

template <typename Pool>
class PoolTransactionSnapshot {
    using Snapshot = std::decay_t<decltype(
        std::declval<const Pool&>().mutable_snapshot()
    )>;

public:
    explicit PoolTransactionSnapshot(const Pool& pool)
        : snapshot_(pool.mutable_snapshot()) {}

    void restore(Pool& pool) const {
        pool.restore_mutable(snapshot_);
    }

private:
    Snapshot snapshot_;
};

} // namespace harness
} // namespace arb
