#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include <boost/json.hpp>
#include "core/json_utils.hpp"

namespace arb::harness {
template <typename T>
struct YbInitialState {
    uint64_t source_block{0};
    uint64_t source_timestamp{0};
    std::string block_hash;
    T leverage{};
    T fee{};
    T collateral{};
    T debt{};
    T rate{};
    T rate_mul{};
    uint64_t rate_time{0};
    T minted{};
    T redeemed{};
    T stable_balance{};
    T lt_stable_balance{};
    T flash_max_loan{};
    T stable_aggregator{};
    T rounding_discount{};
    T lt_donation_discount{};
    bool killed{false};
};
template <typename T>
void validate_yb_initial_state(const YbInitialState<T>& state) {
    const auto finite_nonnegative = [](const T& value) {
        return std::isfinite(value) && value >= T(0);
    };
    const auto finite_positive = [&](const T& value) {
        return finite_nonnegative(value) && value > T(0);
    };
    if (state.source_block == 0 || state.source_timestamp == 0 ||
        state.rate_time == 0 || state.rate_time > state.source_timestamp) {
        throw std::invalid_argument("yb_initial_state has invalid checkpoint timestamps");
    }
    if (state.block_hash.size() != 66 || state.block_hash.substr(0, 2) != "0x") {
        throw std::invalid_argument("yb_initial_state block_hash must be 0x plus 64 hex digits");
    }
    for (std::size_t i = 2; i < state.block_hash.size(); ++i) {
        const char c = state.block_hash[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) {
            throw std::invalid_argument("yb_initial_state block_hash must be 0x plus 64 hex digits");
        }
    }
    if (state.leverage != T(2) || !finite_positive(state.collateral) ||
        !finite_positive(state.debt) || !finite_positive(state.rate_mul) ||
        !finite_positive(state.stable_aggregator) || !finite_nonnegative(state.rate) ||
        !finite_nonnegative(state.minted) || !finite_nonnegative(state.redeemed) ||
        !finite_nonnegative(state.stable_balance) ||
        !finite_nonnegative(state.lt_stable_balance) ||
        !finite_nonnegative(state.flash_max_loan)) {
        throw std::invalid_argument("yb_initial_state contains invalid state quantities");
    }
    if (!finite_nonnegative(state.fee) || state.fee > T(1) ||
        !finite_nonnegative(state.rounding_discount) || state.rounding_discount >= T(1) ||
        !finite_nonnegative(state.lt_donation_discount) || state.lt_donation_discount > T(1)) {
        throw std::invalid_argument("yb_initial_state contains invalid fractions");
    }
}
inline const boost::json::value& required_yb_field(
    const boost::json::object& object, const char* key) {
    const auto* value = object.if_contains(key);
    if (value == nullptr) throw std::invalid_argument(std::string("yb_initial_state missing field: ") + key);
    return *value;
}
template <typename T>
YbInitialState<T> parse_yb_initial_state(const boost::json::value& value) {
    if (!value.is_object()) throw std::invalid_argument("yb_initial_state must be an object");
    const auto& object = value.as_object();
    constexpr std::array<std::string_view, 19> fields{
        "source_block", "source_timestamp", "block_hash", "leverage", "fee",
        "collateral", "debt", "rate", "rate_mul", "rate_time", "minted", "redeemed",
        "stable_balance", "lt_stable_balance", "flash_max_loan", "stable_aggregator",
        "rounding_discount", "lt_donation_discount", "killed",
    };
    for (const auto& item : object) {
        const std::string_view key(item.key().data(), item.key().size());
        if (std::find(fields.begin(), fields.end(), key) == fields.end()) {
            throw std::invalid_argument("unknown yb_initial_state field: " + std::string(key));
        }
    }
    const auto uint_field = [&](const char* key) -> uint64_t {
        const auto& raw = required_yb_field(object, key);
        if (raw.is_uint64()) return raw.as_uint64();
        if (raw.is_int64() && raw.as_int64() >= 0) return static_cast<uint64_t>(raw.as_int64());
        throw std::invalid_argument(std::string("yb_initial_state ") + key + " must be uint64");
    };
    const auto real_field = [&](const char* key) -> T {
        return static_cast<T>(parse_input_double(required_yb_field(object, key))); };
    YbInitialState<T> state;
    state.source_block = uint_field("source_block");
    state.source_timestamp = uint_field("source_timestamp");
    const auto& hash = required_yb_field(object, "block_hash");
    if (!hash.is_string()) throw std::invalid_argument("yb_initial_state block_hash must be a string");
    state.block_hash = std::string(hash.as_string());
    state.leverage = real_field("leverage");
    state.fee = real_field("fee");
    state.collateral = real_field("collateral");
    state.debt = real_field("debt");
    state.rate = real_field("rate");
    state.rate_mul = real_field("rate_mul");
    state.rate_time = uint_field("rate_time");
    state.minted = real_field("minted");
    state.redeemed = real_field("redeemed");
    state.stable_balance = real_field("stable_balance");
    state.lt_stable_balance = real_field("lt_stable_balance");
    state.flash_max_loan = real_field("flash_max_loan");
    state.stable_aggregator = real_field("stable_aggregator");
    state.rounding_discount = real_field("rounding_discount");
    state.lt_donation_discount = real_field("lt_donation_discount");
    const auto& killed = required_yb_field(object, "killed");
    if (!killed.is_bool()) throw std::invalid_argument("yb_initial_state killed must be boolean");
    state.killed = killed.as_bool();
    validate_yb_initial_state(state);
    return state;
}
template <typename T>
boost::json::object yb_initial_state_json(const YbInitialState<T>& state) {
    return {{"source_block", state.source_block}, {"source_timestamp", state.source_timestamp},
        {"block_hash", state.block_hash}, {"leverage", static_cast<double>(state.leverage)},
        {"fee", static_cast<double>(state.fee)}, {"collateral", static_cast<double>(state.collateral)},
        {"debt", static_cast<double>(state.debt)}, {"rate", static_cast<double>(state.rate)},
        {"rate_mul", static_cast<double>(state.rate_mul)}, {"rate_time", state.rate_time},
        {"minted", static_cast<double>(state.minted)}, {"redeemed", static_cast<double>(state.redeemed)},
        {"stable_balance", static_cast<double>(state.stable_balance)},
        {"lt_stable_balance", static_cast<double>(state.lt_stable_balance)},
        {"flash_max_loan", static_cast<double>(state.flash_max_loan)},
        {"stable_aggregator", static_cast<double>(state.stable_aggregator)},
        {"rounding_discount", static_cast<double>(state.rounding_discount)},
        {"lt_donation_discount", static_cast<double>(state.lt_donation_discount)},
        {"killed", state.killed}};
}
} // namespace arb::harness
