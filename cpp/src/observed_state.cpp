#include "events/observed_state.hpp"
#include "pools/pool_config_parse.hpp"
#include <cmath>
#include <fstream>
#include <stdexcept>

namespace arb::events {
namespace {
uint64_t u64(const boost::json::object& row, const char* key) {
    const auto& v = row.at(key);
    if (v.is_uint64()) return v.as_uint64();
    if (v.is_int64() && v.as_int64() >= 0) return static_cast<uint64_t>(v.as_int64());
    throw std::invalid_argument(std::string(key) + " must be uint64");
}
}
template<class T> ObservedStateTape<T>::ObservedStateTape(std::vector<ObservedState<T>> rows)
    : rows_(std::move(rows)) {
    if (rows_.empty()) throw std::invalid_argument("observed state tape is empty");
    for (size_t i = 0; i < rows_.size(); ++i) {
        const auto& r = rows_[i]; const auto& h = r.pool_init.historical_state;
        if (!r.coverage_start_timestamp || r.coverage_end_timestamp < r.coverage_start_timestamp ||
            r.coverage_start_timestamp != rows_.front().coverage_start_timestamp ||
            r.coverage_end_timestamp != rows_.front().coverage_end_timestamp ||
            r.source_timestamp > r.coverage_end_timestamp)
            throw std::invalid_argument("invalid observed state coverage metadata");
        if (r.source_timestamp >= UINT64_MAX / 1'000'000'000ULL ||
            r.available_ns != (r.source_timestamp + 1) * 1'000'000'000ULL ||
            r.observed_through_timestamp != r.source_timestamp || !h.enabled ||
            h.source_block != r.source_block || h.source_timestamp != r.source_timestamp ||
            r.yb_initial_state.source_block != r.source_block ||
            r.yb_initial_state.source_timestamp != r.source_timestamp)
            throw std::invalid_argument("observed state provenance/publication mismatch");
        if (i && (r.available_ns <= rows_[i-1].available_ns ||
                  r.source_block <= rows_[i-1].source_block))
            throw std::invalid_argument("observed state tape must strictly increase");
        if (r.pool_init.policy_kind != pools::twocrypto_fx::PolicyKind::None)
            throw std::invalid_argument("observed state supports no-policy pools only");
        harness::validate_yb_initial_state(r.yb_initial_state);
    }
}
template<class T> ObservedStateTape<T> load_observed_state_jsonl(const std::string& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("Cannot open observed state tape: " + path);
    std::vector<ObservedState<T>> rows; std::string line; size_t line_no = 0;
    while (std::getline(input, line)) {
        ++line_no;
        try {
            const auto o = boost::json::parse(line).as_object();
            ObservedState<T> r;
            r.available_ns = u64(o, "available_ns"); r.source_block = u64(o, "source_block");
            r.source_timestamp = u64(o, "source_timestamp");
            r.observed_through_timestamp = u64(o, "observed_through_timestamp");
            r.coverage_start_timestamp = u64(o, "coverage_start_timestamp");
            r.coverage_end_timestamp = u64(o, "coverage_end_timestamp");
            trading::Costs<T> ignored_costs;
            pools::parse_pool_entry<T>(o.at("pool_init").as_object(), r.pool_init, ignored_costs);
            r.yb_initial_state = harness::parse_yb_initial_state<T>(o.at("yb_initial_state"));
            rows.push_back(std::move(r));
        } catch (const std::exception& e) {
            throw std::runtime_error(path + ":" + std::to_string(line_no) + ": " + e.what());
        }
    }
    return ObservedStateTape<T>(std::move(rows));
}
template class ObservedStateTape<double>;
template class ObservedStateTape<long double>;
template class ObservedStateTape<float>;
template ObservedStateTape<double> load_observed_state_jsonl<double>(const std::string&);
template ObservedStateTape<long double> load_observed_state_jsonl<long double>(const std::string&);
template ObservedStateTape<float> load_observed_state_jsonl<float>(const std::string&);
} // namespace arb::events
