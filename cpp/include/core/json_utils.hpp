// JSON parsing and serialization utilities
#pragma once

#include <boost/json.hpp>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>


namespace arb {

template <typename T>
std::string canonical_float_string(T value) {
    static_assert(std::is_floating_point_v<T>);
    if (value == T(0)) return "0";

    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::scientific
        << std::setprecision(std::numeric_limits<T>::max_digits10 - 1)
        << value;
    return out.str();
}

// Pool/config inputs have a binary64 precision boundary, independent of the
// pool's arithmetic type.  A long-double simulation widens the resulting
// double; it must not recover extra bits by parsing the source decimal as long
// double.  This keeps template-file and JSON-request initialization identical.
inline double parse_input_double(const boost::json::value& v) {
    double value = 0.0;
    if (v.is_string()) {
        const std::string text(v.as_string().c_str());
        char* end = nullptr;
        value = std::strtod(text.c_str(), &end);
        if (end == text.c_str() || *end != '\0') {
            throw std::runtime_error("real input must be a base-10 number");
        }
    } else if (v.is_double()) {
        value = v.as_double();
    } else if (v.is_int64()) {
        value = static_cast<double>(v.as_int64());
    } else if (v.is_uint64()) {
        value = static_cast<double>(v.as_uint64());
    } else {
        throw std::runtime_error("real input must be a number or numeric string");
    }
    if (!std::isfinite(value)) {
        throw std::runtime_error("real input must materialize to finite binary64");
    }
    return value;
}

// ============================================================================
// JSON value parsing (boost::json::value -> T)
// ============================================================================

// Parse a JSON value as a plain real number (no scaling)
template <typename T>
inline T parse_plain_real(const boost::json::value& v) {
    return static_cast<T>(parse_input_double(v));
}

// Parse a JSON value as a real number, scaling down from 1e18 representation
template <typename T>
inline T parse_scaled_1e18(const boost::json::value& v) {
    return static_cast<T>(parse_input_double(v) / 1e18);
}

// Parse fee value: if integer > 1, assume 1e10 representation (e.g. 5000000 -> 0.0005)
// if float < 1, assume direct fraction (e.g. 0.0005)
template <typename T>
inline T parse_fee_1e10(const boost::json::value& v) {
    double d = parse_input_double(v);
    if (d > 1.0) {
        return static_cast<T>(d / 1e10);
    }
    return static_cast<T>(d);
}

// ============================================================================
// Safe JSON extraction helpers
// ============================================================================

inline uint64_t get_u64_opt(const boost::json::object& obj, const char* key, uint64_t fallback) {
    auto it = obj.find(key);
    if (it == obj.end()) return fallback;
    const auto& v = it->value();
    if (v.is_uint64()) return v.as_uint64();
    if (v.is_int64())  return static_cast<uint64_t>(v.as_int64());
    if (v.is_double()) return static_cast<uint64_t>(v.as_double());
    if (v.is_string()) {
        try {
            return std::stoull(std::string(v.as_string().c_str()));
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

template <typename UInt>
bool parse_bounded_uint_field(
    const boost::json::object& obj,
    const char* key,
    UInt fallback,
    UInt& result
) {
    static_assert(std::is_integral_v<UInt> && std::is_unsigned_v<UInt>);
    const auto* value = obj.if_contains(key);
    if (value == nullptr) {
        result = fallback;
        return true;
    }

    uint64_t parsed = 0;
    if (value->is_uint64()) {
        parsed = value->as_uint64();
    } else if (value->is_int64() && value->as_int64() >= 0) {
        parsed = static_cast<uint64_t>(value->as_int64());
    } else {
        return false;
    }
    if (parsed > static_cast<uint64_t>(std::numeric_limits<UInt>::max())) {
        return false;
    }
    result = static_cast<UInt>(parsed);
    return true;
}

inline double get_double_opt(const boost::json::object& obj, const char* key, double fallback) {
    auto it = obj.find(key);
    if (it == obj.end()) return fallback;
    const auto& v = it->value();
    if (v.is_double() || v.is_int64() || v.is_uint64() || v.is_string()) {
        return parse_input_double(v);
    }
    return fallback;
}

inline std::string get_string_opt(const boost::json::object& obj, const char* key, const std::string& fallback) {
    auto it = obj.find(key);
    if (it == obj.end() || !it->value().is_string()) return fallback;
    return std::string(it->value().as_string().c_str());
}

// Write a JSON object to a stream as a single canonical line (no pretty
// printing, no trailing newline). Serialization uses Boost.JSON's shortest
// round-trip floating-point formatting, which is deterministic across runs.
inline void write_json_plain(std::ostream& os, const boost::json::object& obj) {
    os << boost::json::serialize(obj);
}

} // namespace arb
