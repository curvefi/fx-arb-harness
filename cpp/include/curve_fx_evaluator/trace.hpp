#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "curve_fx_evaluator/types.hpp"
#include "harness/actions.hpp"
#include "harness/detailed_output.hpp"

namespace curve_fx::evaluator {

class TraceArena {
public:
    class Lease {
    public:
        Lease(TraceArena& arena, std::unique_lock<std::mutex> lock)
            : arena_(arena), lock_(std::move(lock)) {}

        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&&) = default;
        Lease& operator=(Lease&&) = delete;

        std::vector<arb::harness::Action<RealT>>& actions() {
            return arena_.actions_;
        }
        std::vector<arb::harness::DetailedEntry<RealT>>& detailed_entries() {
            return arena_.detailed_entries_;
        }

        const std::vector<arb::harness::Action<RealT>>& actions() const {
            return arena_.actions_;
        }
        const std::vector<arb::harness::DetailedEntry<RealT>>& detailed_entries() const {
            return arena_.detailed_entries_;
        }

    private:
        TraceArena& arena_;
        std::unique_lock<std::mutex> lock_;
    };

    TraceArena() = default;
    TraceArena(const TraceArena&) = delete;
    TraceArena& operator=(const TraceArena&) = delete;

    Lease acquire() {
        std::unique_lock<std::mutex> lock(mutex_);
        actions_.clear();
        detailed_entries_.clear();
        return Lease(*this, std::move(lock));
    }

    static TraceArena& global_instance();

private:
    friend class Lease;
    std::mutex mutex_;
    std::vector<arb::harness::Action<RealT>> actions_;
    std::vector<arb::harness::DetailedEntry<RealT>> detailed_entries_;
};

std::string serialize_detailed_entries_json(
    const std::vector<arb::harness::DetailedEntry<RealT>>& entries
);

std::string serialize_actions_json(
    const std::vector<arb::harness::Action<RealT>>& actions
);

} // namespace curve_fx::evaluator
