#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <boost/json.hpp>
#include "pools/pool_init.hpp"
#include "harness/yb_initial_state.hpp"

namespace arb::events {
template<class T> struct ObservedState {
    uint64_t available_ns{}, source_block{}, source_timestamp{}, observed_through_timestamp{};
    uint64_t coverage_start_timestamp{}, coverage_end_timestamp{};
    pools::PoolInit<T> pool_init;
    harness::YbInitialState<T> yb_initial_state;
};
template<class T> class ObservedStateTape {
public:
    explicit ObservedStateTape(std::vector<ObservedState<T>> rows);
    const auto& rows() const { return rows_; }
private:
    std::vector<ObservedState<T>> rows_;
};
template<class T> ObservedStateTape<T> load_observed_state_jsonl(const std::string& path);
} // namespace arb::events
