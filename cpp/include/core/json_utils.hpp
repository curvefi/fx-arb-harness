// JSON parsing and serialization utilities
#pragma once

#include <boost/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include "core/sha256.hpp"

namespace arb {

// ============================================================================
// Scaling constants
// ============================================================================

constexpr long double WAD = 1e18L;
constexpr long double FEE_SCALE = 1e10L;

// Pool/config inputs have a binary64 precision boundary, independent of the
// pool's arithmetic type.  A long-double simulation widens the resulting
// double; it must not recover extra bits by parsing the source decimal as long
// double.  This keeps template-file and JSON-request initialization identical.
inline double parse_input_double(const boost::json::value& v) {
    if (v.is_string()) return std::strtod(v.as_string().c_str(), nullptr);
    if (v.is_double()) return v.as_double();
    if (v.is_int64()) return static_cast<double>(v.as_int64());
    if (v.is_uint64()) return static_cast<double>(v.as_uint64());
    return 0.0;
}

// ============================================================================
// Output formatting (value -> string)
// ============================================================================

// Convert a floating-point value to a string representation scaled by 1e18 (wei format)
template <typename T>
inline std::string to_str_1e18(T v) {
    long double scaled = static_cast<long double>(v) * WAD;
    if (!std::isfinite(scaled)) scaled = 0;
    if (scaled < 0) scaled = 0;
    const auto rounded = std::floor(scaled + 0.5L);
    std::ostringstream oss;
    oss.setf(std::ios::fixed);
    oss.precision(0);
    oss << rounded;
    return oss.str();
}

// Convert a floating-point value to a rounded integer string (no scaling)
template <typename T>
inline std::string to_int_string(T v) {
    long double x = static_cast<long double>(v);
    if (!std::isfinite(x)) x = 0;
    if (x < 0) x = 0;
    const auto rounded = std::floor(x + 0.5L);
    std::ostringstream oss;
    oss.setf(std::ios::fixed);
    oss.precision(0);
    oss << rounded;
    return oss.str();
}

// ============================================================================
// JSON value parsing (boost::json::value -> T)
// ============================================================================

// Parse a JSON value as a plain real number (no scaling)
template <typename T>
inline T parse_plain_real(const boost::json::value& v) {
    if constexpr (std::is_floating_point_v<T>) {
        return static_cast<T>(parse_input_double(v));
    } else {
        if (v.is_string()) return static_cast<T>(std::strtold(v.as_string().c_str(), nullptr));
        if (v.is_double()) return static_cast<T>(v.as_double());
        if (v.is_int64())  return static_cast<T>(v.as_int64());
        if (v.is_uint64()) return static_cast<T>(v.as_uint64());
        return T(0);
    }
}

// Parse a JSON value as a real number, scaling down from 1e18 representation
template <typename T>
inline T parse_scaled_1e18(const boost::json::value& v) {
    if constexpr (std::is_floating_point_v<T>) {
        return static_cast<T>(parse_input_double(v) / 1e18);
    } else {
        if (v.is_string()) return static_cast<T>(std::strtold(v.as_string().c_str(), nullptr) / WAD);
        if (v.is_double()) return static_cast<T>(v.as_double() / WAD);
        if (v.is_int64())  return static_cast<T>(static_cast<long double>(v.as_int64()) / WAD);
        if (v.is_uint64()) return static_cast<T>(static_cast<long double>(v.as_uint64()) / WAD);
        return T(0);
    }
}

// Parse fee value: if integer > 1, assume 1e10 representation (e.g. 5000000 -> 0.0005)
// if float < 1, assume direct fraction (e.g. 0.0005)
template <typename T>
inline T parse_fee_1e10(const boost::json::value& v) {
    if constexpr (std::is_floating_point_v<T>) {
        double d = parse_input_double(v);
        if (d > 1.0) {
            return static_cast<T>(d / 1e10);
        }
        return static_cast<T>(d);
    } else {
        if (v.is_int64() || v.is_uint64()) {
            return static_cast<T>(parse_plain_real<T>(v));
        }
        if (v.is_string()) {
            std::string s(v.as_string().c_str());
            if (s.find('.') == std::string::npos) {
                return static_cast<T>(std::strtold(s.c_str(), nullptr));
            }
            return static_cast<T>(std::strtold(s.c_str(), nullptr) * FEE_SCALE);
        }
        return static_cast<T>(v.as_double() * FEE_SCALE);
    }
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

inline double get_double_opt(const boost::json::object& obj, const char* key, double fallback) {
    auto it = obj.find(key);
    if (it == obj.end()) return fallback;
    const auto& v = it->value();
    if (v.is_double()) return v.as_double();
    if (v.is_int64())  return static_cast<double>(v.as_int64());
    if (v.is_uint64()) return static_cast<double>(v.as_uint64());
    if (v.is_string()) {
        try {
            return std::stod(std::string(v.as_string().c_str()));
        } catch (...) {
            return fallback;
        }
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

namespace detection_for_canonical {

inline void canonical_append(std::string& out, const boost::json::value& v);

inline void canonical_append_string(std::string& out, const boost::json::string_view& s) {
    out += '"';
    for (char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

inline void canonical_append(std::string& out, const boost::json::value& v) {
    if (v.is_null()) {
        out += "null";
    } else if (v.is_bool()) {
        out += v.as_bool() ? "true" : "false";
    } else if (v.is_int64() || v.is_uint64() || v.is_double()) {
        out += boost::json::serialize(v);
    } else if (v.is_string()) {
        canonical_append_string(out, v.as_string());
    } else if (v.is_array()) {
        out += '[';
        bool first = true;
        for (const auto& item : v.as_array()) {
            if (!first) out += ',';
            first = false;
            canonical_append(out, item);
        }
        out += ']';
    } else if (v.is_object()) {
        out += '{';
        std::map<std::string, std::string> sorted;
        for (const auto& kv : v.as_object()) {
            std::string key(kv.key().data(), kv.key().size());
            std::string val;
            canonical_append(val, kv.value());
            sorted.emplace(std::move(key), std::move(val));
        }
        bool first = true;
        for (const auto& [key, val] : sorted) {
            if (!first) out += ',';
            first = false;
            canonical_append_string(out, key);
            out += ':';
            out += val;
        }
        out += '}';
    } else {
        out += "null";
    }
}

} // namespace detection_for_canonical

// Deterministic, key-sorted JSON serialization (canonical form) of any value.
inline std::string canonical_json(const boost::json::value& v) {
    std::string out;
    detection_for_canonical::canonical_append(out, v);
    return out;
}

// SHA-256 (hex) of the canonical JSON serialization of a value.
inline std::string sha256_canonical_json(const boost::json::value& v) {
    const std::string canonical = canonical_json(v);
    return crypto::sha256_hex(canonical.data(), canonical.size());
}

} // namespace arb
