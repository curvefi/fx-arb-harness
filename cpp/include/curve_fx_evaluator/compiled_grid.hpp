#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <boost/json.hpp>

#include "core/json_utils.hpp"
#include "curve_fx_evaluator/evaluator.hpp"
#include "pools/pool_config_parse.hpp"

namespace curve_fx::evaluator {

namespace detail {

inline bool parse_grid_index(std::string_view value, size_t& result) {
    if (value.empty() || !std::all_of(value.begin(), value.end(), [](char c) {
            return c >= '0' && c <= '9';
        })) {
        return false;
    }
    try {
        const auto parsed = std::stoull(std::string(value));
        if (parsed > std::numeric_limits<size_t>::max()) return false;
        result = static_cast<size_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

inline std::vector<std::string> grid_path_parts(
    std::string_view path,
    std::string& error
) {
    std::vector<std::string> parts;
    size_t begin = 0;
    while (begin <= path.size()) {
        const size_t end = path.find('.', begin);
        const size_t stop = end == std::string_view::npos ? path.size() : end;
        if (stop == begin) {
            error = "grid axis contains an invalid dotted path";
            return {};
        }
        parts.emplace_back(path.substr(begin, stop - begin));
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    return parts;
}

inline bool apply_grid_update(
    boost::json::object& payload,
    std::string_view path,
    const boost::json::value& value,
    std::string& error
) {
    const auto parts = grid_path_parts(path, error);
    if (parts.empty()) return false;

    boost::json::value root(payload);
    boost::json::value* cursor = &root;
    for (size_t index = 0; index + 1 < parts.size(); ++index) {
        const auto& part = parts[index];
        size_t next_index = 0;
        const bool next_is_index = parse_grid_index(parts[index + 1], next_index);
        if (cursor->is_object()) {
            auto& object = cursor->as_object();
            auto* child = object.if_contains(part);
            if (child == nullptr) {
                object[part] = next_is_index
                    ? boost::json::value(boost::json::array{})
                    : boost::json::value(boost::json::object{});
                child = object.if_contains(part);
            }
            cursor = child;
        } else if (cursor->is_array()) {
            size_t position = 0;
            if (!parse_grid_index(part, position) ||
                position >= cursor->as_array().size()) {
                error = "grid axis list index is out of range: " + part;
                return false;
            }
            cursor = &cursor->as_array()[position];
        } else {
            error = "grid axis collides with a scalar at: " + part;
            return false;
        }
    }

    const auto& leaf = parts.back();
    if (cursor->is_object()) {
        cursor->as_object()[leaf] = value;
    } else if (cursor->is_array()) {
        size_t position = 0;
        if (!parse_grid_index(leaf, position) ||
            position >= cursor->as_array().size()) {
            error = "grid axis list index is out of range: " + leaf;
            return false;
        }
        cursor->as_array()[position] = value;
    } else {
        error = "grid axis collides with a scalar at: " + leaf;
        return false;
    }
    payload = std::move(root.as_object());
    return true;
}

inline bool unsigned_json(const boost::json::value& value, uint64_t& out) {
    if (value.is_uint64()) {
        out = value.as_uint64();
        return true;
    }
    if (value.is_int64() && value.as_int64() >= 0) {
        out = static_cast<uint64_t>(value.as_int64());
        return true;
    }
    return false;
}

inline std::string grid_candidate_id(uint64_t ordinal) {
    std::ostringstream stream;
    stream << 'p';
    stream.width(8);
    stream.fill('0');
    stream << ordinal;
    return stream.str();
}

inline bool contains_policy_override(const boost::json::object& pool) {
    if (pool.if_contains("policy") != nullptr) return true;
    const auto* nested = pool.if_contains("pool");
    return nested != nullptr && nested->is_object() &&
        nested->as_object().if_contains("policy") != nullptr;
}

} // namespace detail

template <typename T>
class CompiledGrid {
public:
    static std::optional<CompiledGrid> compile(
        const boost::json::object& grid,
        size_t expected_policy_params,
        std::string& error
    ) {
        const auto* defaults_value = grid.if_contains("candidate_defaults");
        const auto* axes_value = grid.if_contains("axes");
        const auto* order_value = grid.if_contains("axis_order");
        const auto* shape_value = grid.if_contains("shape");
        if (defaults_value == nullptr || !defaults_value->is_object() ||
            axes_value == nullptr || !axes_value->is_object() ||
            order_value == nullptr || !order_value->is_array() ||
            shape_value == nullptr || !shape_value->is_array()) {
            error = "grid requires candidate_defaults, axes, axis_order, and shape";
            return std::nullopt;
        }

        boost::json::object defaults = defaults_value->as_object();
        for (const auto& field : defaults) {
            if (field.key() != "policy_params" && field.key() != "pool") {
                error = "grid candidate defaults contain unknown field: " +
                    std::string(field.key());
                return std::nullopt;
            }
        }
        const auto* policy = defaults.if_contains("policy_params");
        const auto* pool = defaults.if_contains("pool");
        if (policy == nullptr || !policy->is_array() ||
            pool == nullptr || !pool->is_object()) {
            error = "grid candidate defaults require policy_params and pool";
            return std::nullopt;
        }
        if (detail::contains_policy_override(pool->as_object())) {
            error = "grid pool policy is prohibited; use policy_params";
            return std::nullopt;
        }

        CompiledGrid compiled;
        if (!compiled.parse_policy_values(
                policy->as_array(), expected_policy_params,
                compiled.base_policy_params_, error)) {
            return std::nullopt;
        }
        try {
            compiled.base_pool_override_ =
                arb::pools::parse_pool_override<T>(pool->as_object());
        } catch (const std::exception& exception) {
            error = exception.what();
            return std::nullopt;
        }

        const auto& axes = axes_value->as_object();
        const auto& order = order_value->as_array();
        const auto& shape = shape_value->as_array();
        if (order.size() != axes.size() || shape.size() != order.size()) {
            error = "grid axis_order, axes, and shape must describe the same axes";
            return std::nullopt;
        }

        uint64_t total = 1;
        std::unordered_set<std::string> seen_names;
        compiled.axes_.reserve(order.size());
        for (size_t axis_index = 0; axis_index < order.size(); ++axis_index) {
            if (!order[axis_index].is_string()) {
                error = "grid axis_order entries must be strings";
                return std::nullopt;
            }
            const std::string name(order[axis_index].as_string().c_str());
            if (!seen_names.insert(name).second) {
                error = "grid axis_order entries must be unique";
                return std::nullopt;
            }
            const auto* values = axes.if_contains(name);
            uint64_t declared = 0;
            if (values == nullptr || !values->is_array() || values->as_array().empty() ||
                !detail::unsigned_json(shape[axis_index], declared) || declared == 0 ||
                declared != values->as_array().size() ||
                total > (static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) + 1) /
                    declared) {
                error = "grid shape does not match its axis values";
                return std::nullopt;
            }
            total *= declared;

            std::vector<AxisValue> compiled_axis;
            compiled_axis.reserve(values->as_array().size());
            for (const auto& axis_value : values->as_array()) {
                AxisValue value;
                std::vector<std::pair<std::string, const boost::json::value*>> updates;
                if (axis_value.is_object()) {
                    updates.reserve(axis_value.as_object().size());
                    for (const auto& update : axis_value.as_object()) {
                        updates.emplace_back(
                            std::string(update.key()), &update.value());
                    }
                } else {
                    updates.emplace_back(name, &axis_value);
                }
                if (updates.empty()) {
                    error = "grid axis mapping values must not be empty";
                    return std::nullopt;
                }
                boost::json::object pool_patch_payload;
                pool_patch_payload["pool"] = boost::json::object{};
                for (const auto& [path, update] : updates) {
                    const auto parts = detail::grid_path_parts(path, error);
                    if (parts.empty()) return std::nullopt;
                    if (parts.front() == "policy_params") {
                        if (parts.size() == 2) {
                            size_t index = 0;
                            if (!detail::parse_grid_index(parts[1], index) ||
                                index >= expected_policy_params) {
                                error = "grid policy parameter index is out of range";
                                return std::nullopt;
                            }
                            if (!update->is_double() && !update->is_int64() &&
                                !update->is_uint64()) {
                                error = "every grid policy parameter must be numeric";
                                return std::nullopt;
                            }
                            value.policy_updates.emplace_back(
                                index,
                                static_cast<T>(arb::parse_input_double(*update)));
                        } else {
                            error = "grid policy parameter path is invalid";
                            return std::nullopt;
                        }
                    } else if (parts.front() == "pool") {
                        if (parts.size() < 2) {
                            error = "grid pool parameter path is invalid";
                            return std::nullopt;
                        }
                        if (!detail::apply_grid_update(
                                pool_patch_payload, path, *update, error)) {
                            return std::nullopt;
                        }
                    } else {
                        error = "grid axes must update policy_params or pool";
                        return std::nullopt;
                    }
                }

                boost::json::object pool_patch =
                    pool_patch_payload.at("pool").as_object();
                if (!pool_patch.empty()) {
                    if (detail::contains_policy_override(pool_patch)) {
                        error = "grid pool policy is prohibited; use policy_params";
                        return std::nullopt;
                    }
                    try {
                        value.pool_patch =
                            arb::pools::parse_pool_override<T>(pool_patch);
                    } catch (const std::exception& exception) {
                        error = exception.what();
                        return std::nullopt;
                    }
                }
                compiled_axis.push_back(std::move(value));
            }
            compiled.axes_.push_back(std::move(compiled_axis));
        }
        compiled.total_ = total;
        return compiled;
    }

    bool materialize_ranges(
        const boost::json::array& ranges,
        size_t max_candidates,
        std::vector<EvaluationCandidate<T>>& candidates,
        std::string& error
    ) const {
        if (ranges.empty()) {
            error = "grid ranges must not be empty";
            return false;
        }
        uint64_t previous_stop = 0;
        bool first = true;
        size_t count = 0;
        std::vector<std::pair<uint64_t, uint64_t>> parsed;
        parsed.reserve(ranges.size());
        for (const auto& item : ranges) {
            if (!item.is_array() || item.as_array().size() != 2) {
                error = "grid ranges must contain [start, count] pairs";
                return false;
            }
            uint64_t start = 0;
            uint64_t length = 0;
            if (!detail::unsigned_json(item.as_array()[0], start) ||
                !detail::unsigned_json(item.as_array()[1], length) || length == 0 ||
                start >= total_ || length > total_ - start ||
                (!first && start < previous_stop) ||
                length > max_candidates || count > max_candidates - length) {
                error = "grid ranges must be ordered, disjoint, in range, and within the batch limit";
                return false;
            }
            parsed.emplace_back(start, length);
            previous_stop = start + length;
            first = false;
            count += static_cast<size_t>(length);
        }

        candidates.clear();
        candidates.reserve(count);
        std::vector<size_t> coordinates(axes_.size());
        for (const auto& [start, length] : parsed) {
            for (uint64_t ordinal = start; ordinal < start + length; ++ordinal) {
                EvaluationCandidate<T> candidate;
                candidate.ordinal = static_cast<uint32_t>(ordinal);
                candidate.candidate_id = detail::grid_candidate_id(ordinal);
                candidate.policy_params = base_policy_params_;
                candidate.typed_pool_override = base_pool_override_;

                uint64_t remainder = ordinal;
                for (size_t reverse = axes_.size(); reverse-- > 0;) {
                    coordinates[reverse] =
                        static_cast<size_t>(remainder % axes_[reverse].size());
                    remainder /= axes_[reverse].size();
                }
                for (size_t axis = 0; axis < axes_.size(); ++axis) {
                    const auto& value = axes_[axis][coordinates[axis]];
                    for (const auto& [index, parameter] : value.policy_updates) {
                        candidate.policy_params[index] = parameter;
                    }
                    if (value.pool_patch.has_value()) {
                        candidate.typed_pool_override->overlay(*value.pool_patch);
                    }
                }
                candidates.push_back(std::move(candidate));
            }
        }
        return true;
    }

    uint64_t size() const { return total_; }

private:
    struct AxisValue {
        std::vector<std::pair<size_t, T>> policy_updates;
        std::optional<arb::pools::PoolOverride<T>> pool_patch;
    };
    bool parse_policy_values(
        const boost::json::array& values,
        size_t expected,
        std::vector<T>& output,
        std::string& error
    ) const {
        if (values.size() != expected) {
            error = "grid policy parameter count mismatch";
            return false;
        }
        output.clear();
        output.reserve(values.size());
        for (const auto& value : values) {
            if (!value.is_double() && !value.is_int64() && !value.is_uint64()) {
                error = "every grid policy parameter must be numeric";
                return false;
            }
            output.push_back(static_cast<T>(arb::parse_input_double(value)));
        }
        return true;
    }

    std::vector<T> base_policy_params_;
    arb::pools::PoolOverride<T> base_pool_override_;
    std::vector<std::vector<AxisValue>> axes_;
    uint64_t total_{0};
};

} // namespace curve_fx::evaluator
