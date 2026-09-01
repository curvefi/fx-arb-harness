// Templated pool config parsing.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

#include <boost/json.hpp>

#include "core/json_utils.hpp"
#include "pools/pool_init.hpp"
#include "pools/twocrypto_fx/twocrypto.hpp"
#include "trading/costs.hpp"

namespace arb {
namespace pools {

inline bool is_number_or_string(const boost::json::value& v) {
    return v.is_string() || v.is_double() || v.is_int64() || v.is_uint64();
}

inline void reject_unknown_fields(
    const boost::json::object& object,
    std::initializer_list<std::string_view> allowed,
    std::string_view context
) {
    for (const auto& item : object) {
        const std::string_view key(item.key().data(), item.key().size());
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
            throw std::runtime_error(
                std::string(context) + " contains unknown field: " +
                std::string(key)
            );
        }
    }
}

// Lossless scalar -> string for the uint256 parity path: integer literals stay
// exact and doubles round-trip at full precision (matches the parity harness).
inline std::string policy_scalar_to_string(const boost::json::value& v) {
    if (v.is_string()) return std::string(v.as_string().c_str());
    if (v.is_int64()) return std::to_string(v.as_int64());
    if (v.is_uint64()) return std::to_string(v.as_uint64());
    if (v.is_double()) {
        std::ostringstream oss;
        oss.precision(17);
        oss << v.as_double();
        return oss.str();
    }
    return "0";
}

template <typename T>
T parse_config_wad(const boost::json::value& v) {
    if constexpr (std::is_same_v<T, twocrypto_fx::uint256>) {
        return twocrypto_fx::uint256(policy_scalar_to_string(v));
    } else {
        return parse_scaled_1e18<T>(v);
    }
}

enum class PoolEntryUnits {
    ContractScaled,
    CandidateHuman,
};

template <typename T>
T parse_ratio_wad(
    const boost::json::value& v,
    PoolEntryUnits units,
    std::string_view field
) {
    if constexpr (std::is_same_v<T, twocrypto_fx::uint256>) {
        return parse_config_wad<T>(v);
    } else {
        const T value = units == PoolEntryUnits::CandidateHuman
            ? parse_plain_real<T>(v)
            : parse_config_wad<T>(v);
        if (value < T(0) || value > T(1)) {
            throw std::runtime_error(
                std::string(field) + " must be a fraction in [0, 1]"
            );
        }
        if (value != T(0) && value < T(1e-18)) {
            throw std::runtime_error(
                std::string(field) +
                " is below one WAD unit; check candidate/template units"
            );
        }
        return value;
    }
}

template <typename T>
T parse_config_plain(const boost::json::value& v) {
    if constexpr (std::is_same_v<T, twocrypto_fx::uint256>) {
        return twocrypto_fx::uint256(policy_scalar_to_string(v));
    } else {
        return parse_plain_real<T>(v);
    }
}

template <typename T>
T parse_config_fee(const boost::json::value& v) {
    if constexpr (std::is_same_v<T, twocrypto_fx::uint256>) {
        return twocrypto_fx::uint256(policy_scalar_to_string(v));
    } else {
        return parse_fee_1e10<T>(v);
    }
}

template <typename T>
T parse_fee_value(const boost::json::value& v, std::string_view field) {
    const T value = parse_config_fee<T>(v);
    const T precision = twocrypto_fx::PoolTraits<T>::FEE_PRECISION();
    if (value < T(0) || value > precision) {
        throw std::runtime_error(
            std::string(field) + " must be a fraction in [0, 1]"
        );
    }
    if constexpr (std::is_floating_point_v<T>) {
        if (value != T(0) && value < T(1e-10)) {
            throw std::runtime_error(
                std::string(field) +
                " is below one fee-precision unit; check candidate/template units"
            );
        }
    }
    return value;
}

template <typename T>
T parse_historical_wad(const boost::json::value& v) {
    if (!is_number_or_string(v)) {
        throw std::runtime_error("historical_state WAD values must be numbers or strings");
    }
    if constexpr (std::is_same_v<T, twocrypto_fx::uint256>) {
        return twocrypto_fx::uint256(policy_scalar_to_string(v));
    } else {
        return parse_scaled_1e18<T>(v);
    }
}

template <typename T>
T parse_historical_plain(const boost::json::value& v) {
    if (!is_number_or_string(v)) {
        throw std::runtime_error("historical_state plain values must be numbers or strings");
    }
    if constexpr (std::is_same_v<T, twocrypto_fx::uint256>) {
        return twocrypto_fx::uint256(policy_scalar_to_string(v));
    } else {
        return parse_plain_real<T>(v);
    }
}

inline const boost::json::value& required_historical_field(
    const boost::json::object& obj,
    const char* key
) {
    const auto* value = obj.if_contains(key);
    if (value == nullptr) {
        throw std::runtime_error(std::string("pool historical_state missing field: ") + key);
    }
    return *value;
}

template <typename T>
PoolHistoricalState<T> parse_historical_state(const boost::json::value& value) {
    namespace json = boost::json;
    if (!value.is_object()) {
        throw std::runtime_error("pool historical_state must be an object");
    }
    const auto& obj = value.as_object();
    reject_unknown_fields(
        obj,
        {
            "source_block", "source_timestamp", "balances", "admin_balances",
            "last_admin_fee_claim_timestamp", "D", "total_supply",
            "price_scale", "price_oracle", "last_prices", "last_timestamp",
            "virtual_price", "xcp_profit", "lp_xcp_profit",
            "donation_shares", "last_donation_release_ts",
            "donation_protection_expiry_ts", "donation_protection_period",
            "donation_protection_lp_threshold",
            "donation_protection_extension_remainder",
            "donation_shares_max_ratio"
        },
        "pool historical_state"
    );
    PoolHistoricalState<T> state{};
    state.enabled = true;
    state.source_block = get_u64_opt(obj, "source_block", 0);
    state.source_timestamp = get_u64_opt(obj, "source_timestamp", 0);

    const auto parse_pair = [&](const char* key) {
        const auto& raw = required_historical_field(obj, key);
        if (!raw.is_array() || raw.as_array().size() != 2) {
            throw std::runtime_error(std::string("pool historical_state ") + key + " must have exactly two entries");
        }
        const auto& arr = raw.as_array();
        return std::array<T, 2>{
            parse_historical_wad<T>(arr[0]),
            parse_historical_wad<T>(arr[1]),
        };
    };

    state.balances = parse_pair("balances");
    state.admin_balances = parse_pair("admin_balances");
    state.last_admin_fee_claim_timestamp = get_u64_opt(
        obj, "last_admin_fee_claim_timestamp", 0
    );
    state.D = parse_historical_wad<T>(required_historical_field(obj, "D"));
    state.total_supply = parse_historical_wad<T>(required_historical_field(obj, "total_supply"));
    state.price_scale = parse_historical_wad<T>(required_historical_field(obj, "price_scale"));
    state.price_oracle = parse_historical_wad<T>(required_historical_field(obj, "price_oracle"));
    state.last_prices = parse_historical_wad<T>(required_historical_field(obj, "last_prices"));
    state.last_timestamp = get_u64_opt(obj, "last_timestamp", 0);
    state.virtual_price = parse_historical_wad<T>(required_historical_field(obj, "virtual_price"));
    state.xcp_profit = parse_historical_wad<T>(required_historical_field(obj, "xcp_profit"));
    state.lp_xcp_profit = parse_historical_wad<T>(required_historical_field(obj, "lp_xcp_profit"));
    state.donation_shares = parse_historical_wad<T>(required_historical_field(obj, "donation_shares"));
    state.last_donation_release_ts = parse_historical_plain<T>(required_historical_field(obj, "last_donation_release_ts"));
    state.donation_protection_expiry_ts = parse_historical_plain<T>(required_historical_field(obj, "donation_protection_expiry_ts"));
    state.donation_protection_period = parse_historical_plain<T>(required_historical_field(obj, "donation_protection_period"));
    state.donation_protection_lp_threshold = parse_historical_wad<T>(required_historical_field(obj, "donation_protection_lp_threshold"));
    state.donation_protection_extension_remainder = parse_historical_wad<T>(required_historical_field(obj, "donation_protection_extension_remainder"));
    state.donation_shares_max_ratio = parse_historical_wad<T>(required_historical_field(obj, "donation_shares_max_ratio"));

    if (state.source_timestamp == 0 || state.last_timestamp == 0) {
        throw std::runtime_error("pool historical_state requires nonzero source_timestamp and last_timestamp");
    }
    if (!(state.D > T(0)) || !(state.total_supply > T(0)) ||
        !(state.price_scale > T(0)) || !(state.price_oracle > T(0)) ||
        !(state.virtual_price > T(0))) {
        throw std::runtime_error("pool historical_state requires positive D, supply, prices, and virtual_price");
    }
    return state;
}

template <typename T>
twocrypto_fx::PolicyConfig<T> parse_policy_config(const boost::json::value& policy) {
    twocrypto_fx::PolicyConfig<T> cfg{};
    if (policy.is_string()) {
        cfg.kind = twocrypto_fx::policy_kind_from_string(std::string(policy.as_string().c_str()));
        return cfg;
    }
    if (!policy.is_object()) {
        throw std::runtime_error("pool policy must be a string or object");
    }

    const auto& po = policy.as_object();
    reject_unknown_fields(
        po,
        {"kind", "price_source", "price_source_ema_half_time", "params", "fee", "fee_bps"},
        "pool policy"
    );
    std::string kind = "none";
    if (auto* k = po.if_contains("kind")) {
        if (!k->is_string()) {
            throw std::runtime_error("pool policy kind must be a string");
        }
        kind = std::string(k->as_string().c_str());
    }
    cfg.kind = twocrypto_fx::policy_kind_from_string(kind);

    if (auto* source = po.if_contains("price_source")) {
        if (!source->is_string()) {
            throw std::runtime_error("pool policy price_source must be a string");
        }
        const std::string value(source->as_string().c_str());
        if (!value.empty() && value != "cex" && value != "event_p_cex") {
            throw std::runtime_error(
                "external policy price_source is no longer supported"
            );
        }
    }
    if (po.if_contains("price_source_ema_half_time")) {
        throw std::runtime_error(
            "policy price_source_ema_half_time is no longer supported"
        );
    }
    if (auto* params = po.if_contains("params")) {
        if (!params->is_array()) {
            throw std::runtime_error("pool policy params must be an array of numbers");
        }
        const auto& arr = params->as_array();
        if (arr.size() > cfg.params.size()) {
            throw std::runtime_error("pool policy params: too many entries");
        }
        for (std::size_t i = 0; i < arr.size(); ++i) {
            if (!is_number_or_string(arr[i])) {
                throw std::runtime_error("pool policy params entries must be numbers or strings");
            }
            if constexpr (std::is_same_v<T, twocrypto_fx::uint256>) {
                cfg.params[i] = twocrypto_fx::uint256(policy_scalar_to_string(arr[i]));
            } else {
                cfg.params[i] = parse_plain_real<T>(arr[i]);
            }
        }
        cfg.n_params = arr.size();
    }
    if (auto* fee = po.if_contains("fee")) {
        if (!is_number_or_string(*fee)) {
            throw std::runtime_error("pool policy fee must be a string or number");
        }
        if constexpr (std::is_same_v<T, twocrypto_fx::uint256>) {
            cfg.fee = twocrypto_fx::uint256(policy_scalar_to_string(*fee));
        } else {
            cfg.fee = parse_fee_value<T>(*fee, "pool.policy.fee");
        }
    } else if (auto* fee_bps = po.if_contains("fee_bps")) {
        if (!is_number_or_string(*fee_bps)) {
            throw std::runtime_error("pool policy fee_bps must be a string or number");
        }
        if constexpr (std::is_same_v<T, twocrypto_fx::uint256>) {
            cfg.fee = twocrypto_fx::uint256(policy_scalar_to_string(*fee_bps)) *
                twocrypto_fx::PoolTraits<T>::FEE_PRECISION() / twocrypto_fx::uint256(10000);
        } else {
            // Unit conversion is part of input materialization, so perform it
            // in binary64 before widening to the pool arithmetic type.
            cfg.fee = static_cast<T>(parse_input_double(*fee_bps) / 10000.0);
        }
    }
    return cfg;
}

// Entry format: { "pool": {...}, "costs": {...}, "tag": "..." }
// Or just pool params directly: { "A": ..., "gamma": ..., ... }.
template <typename T>
void parse_pool_entry(
    const boost::json::object& entry,
    PoolInit<T>& out_pool,
    arb::trading::Costs<T>& out_costs,
    PoolEntryUnits units = PoolEntryUnits::ContractScaled
) {
    namespace json = boost::json;

    const bool wrapped_pool = entry.contains("pool");
    if (wrapped_pool) {
        reject_unknown_fields(entry, {"pool", "costs", "tag"}, "pool override");
        if (!entry.at("pool").is_object()) {
            throw std::runtime_error("pool override field 'pool' must be an object");
        }
    }

    const json::object& pool = wrapped_pool
        ? entry.at("pool").as_object()
        : entry;

    reject_unknown_fields(
        pool,
        {
            "tag", "costs", "initial_liquidity", "A", "gamma", "mid_fee",
            "out_fee", "fee_gamma", "adjustment_step_min", "adjustment_step_max",
            "ma_time", "reserved_profit_fraction", "admin_fee", "policy",
            "initial_price", "start_timestamp", "historical_state", "donation_apy",
            "donation_frequency", "donation_duration", "initial_donation_days",
            "donation_coins_ratio", "user_swap_size_frac",
            "lp_profit_fraction", "allowed_extra_profit", "adjustment_step",
            "fee_params", "fee_model_name"
        },
        "pool override"
    );


    if (auto* v = entry.if_contains("tag")) {
        if (v->is_string()) out_pool.tag = v->as_string().c_str();
    }

    if (auto* v = pool.if_contains("initial_liquidity")) {
        const auto& a = v->as_array();
        if (a.size() >= 2) {
            out_pool.initial_liq[0] = parse_config_wad<T>(a[0]);
            out_pool.initial_liq[1] = parse_config_wad<T>(a[1]);
        }
    }

    if (auto* v = pool.if_contains("A")) out_pool.A = parse_config_plain<T>(*v);
    if (auto* v = pool.if_contains("gamma")) out_pool.gamma = parse_config_plain<T>(*v);
    if (auto* v = pool.if_contains("mid_fee")) {
        out_pool.mid_fee = parse_fee_value<T>(*v, "pool.mid_fee");
    }
    if (auto* v = pool.if_contains("out_fee")) {
        out_pool.out_fee = parse_fee_value<T>(*v, "pool.out_fee");
    }
    if (auto* v = pool.if_contains("fee_gamma")) {
        out_pool.fee_gamma = parse_ratio_wad<T>(*v, units, "pool.fee_gamma");
    }
    if (
        pool.if_contains("lp_profit_fraction") ||
        pool.if_contains("allowed_extra_profit") ||
        pool.if_contains("adjustment_step") ||
        pool.if_contains("fee_params") ||
        pool.if_contains("fee_model_name")
    ) {
        throw std::runtime_error("legacy pool config fields are not supported; use reserved_profit_fraction, admin_fee, adjustment_step_min, adjustment_step_max, and policy");
    }
    if (auto* v = pool.if_contains("adjustment_step_min")) {
        out_pool.adjustment_step_min = parse_ratio_wad<T>(
            *v, units, "pool.adjustment_step_min");
    }
    if (auto* v = pool.if_contains("adjustment_step_max")) {
        out_pool.adjustment_step_max = parse_ratio_wad<T>(
            *v, units, "pool.adjustment_step_max");
    }
    if (auto* v = pool.if_contains("ma_time")) out_pool.ma_time = parse_config_plain<T>(*v);
    if (auto* v = pool.if_contains("reserved_profit_fraction")) {
        out_pool.reserved_profit_fraction = parse_fee_value<T>(
            *v, "pool.reserved_profit_fraction");
    }
    if (auto* v = pool.if_contains("admin_fee")) {
        out_pool.admin_fee = parse_fee_value<T>(*v, "pool.admin_fee");
    }
    if (auto* v = pool.if_contains("policy")) {
        out_pool.policy_config = parse_policy_config<T>(*v);
        out_pool.policy_kind = out_pool.policy_config.kind;
    }
    if (auto* v = pool.if_contains("initial_price")) out_pool.initial_price = parse_config_wad<T>(*v);
    if (auto* v = pool.if_contains("start_timestamp")) {
        out_pool.start_ts = static_cast<uint64_t>(parse_plain_real<T>(*v));
        if (out_pool.start_ts > 10000000000ULL) {
            out_pool.start_ts /= 1000ULL;
        }
    }
    if (auto* v = pool.if_contains("historical_state")) {
        out_pool.historical_state = parse_historical_state<T>(*v);
        if (out_pool.start_ts != 0 &&
            out_pool.start_ts < out_pool.historical_state.source_timestamp) {
            throw std::runtime_error(
                "pool start_timestamp predates historical_state source_timestamp"
            );
        }
    }

    if (auto* v = pool.if_contains("donation_apy")) out_pool.donation_apy = parse_plain_real<T>(*v);
    if (auto* v = pool.if_contains("donation_frequency")) out_pool.donation_frequency = parse_plain_real<T>(*v);
    if (auto* v = pool.if_contains("donation_duration")) out_pool.donation_duration = parse_plain_real<T>(*v);
    if (auto* v = pool.if_contains("initial_donation_days")) out_pool.initial_donation_days = parse_plain_real<T>(*v);
    if (auto* v = pool.if_contains("donation_coins_ratio")) {
        T r = parse_plain_real<T>(*v);
        out_pool.donation_coins_ratio = std::clamp<T>(r, T(0), T(1));
    }
    if (auto* v = pool.if_contains("user_swap_size_frac")) {
        out_pool.user_swap_size_frac = std::clamp<T>(
            parse_plain_real<T>(*v), T(0), T(1));
    }
    if (auto* c = entry.if_contains("costs")) {
        if (!c->is_object()) {
            throw std::runtime_error("pool costs override must be an object");
        }
        const auto& co = c->as_object();
        reject_unknown_fields(
            co,
            {"arb_fee_bps", "gas_coin0", "use_volume_cap", "volume_cap_mult", "volume_cap_is_coin_1"},
            "pool costs override"
        );
        if (auto* v = co.if_contains("arb_fee_bps")) out_costs.arb_fee_bps = parse_plain_real<T>(*v);
        if (auto* v = co.if_contains("gas_coin0")) out_costs.gas_coin0 = parse_plain_real<T>(*v);
        if (auto* v = co.if_contains("use_volume_cap")) out_costs.use_volume_cap = v->as_bool();
        if (auto* v = co.if_contains("volume_cap_mult")) out_costs.volume_cap_mult = parse_plain_real<T>(*v);
        if (auto* v = co.if_contains("volume_cap_is_coin_1")) {
            out_costs.volume_cap_is_coin1 = v->is_bool()
                ? v->as_bool()
                : (parse_plain_real<T>(*v) != T(0));
        }
    }
}

// Parsed candidate pool overrides contain only typed values and field masks.
// This lets one candidate's JSON be materialized once and applied read-only to
// each scenario without retaining or copying the source JSON.
template <typename T>
struct PoolOverride {
    enum PoolField : uint32_t {
        Tag = 1u << 0,
        InitialLiquidity = 1u << 1,
        A = 1u << 2,
        Gamma = 1u << 3,
        MidFee = 1u << 4,
        OutFee = 1u << 5,
        FeeGamma = 1u << 6,
        AdjustmentStepMin = 1u << 7,
        AdjustmentStepMax = 1u << 8,
        MaTime = 1u << 9,
        ReservedProfitFraction = 1u << 10,
        AdminFee = 1u << 11,
        Policy = 1u << 12,
        InitialPrice = 1u << 13,
        StartTimestamp = 1u << 14,
        HistoricalState = 1u << 15,
        DonationApy = 1u << 16,
        DonationFrequency = 1u << 17,
        DonationDuration = 1u << 18,
        InitialDonationDays = 1u << 19,
        DonationCoinsRatio = 1u << 20,
        UserSwapSizeFrac = 1u << 21,
    };
    enum CostField : uint32_t {
        ArbFeeBps = 1u << 0,
        GasCoin0 = 1u << 1,
        UseVolumeCap = 1u << 2,
        VolumeCapMult = 1u << 3,
        VolumeCapIsCoin1 = 1u << 4,
    };

    PoolInit<T> pool{};
    arb::trading::Costs<T> costs{};
    std::optional<T> yb_releverage_fee{};
    uint32_t pool_fields{0};
    uint32_t cost_fields{0};

    void apply(PoolInit<T>& target_pool, arb::trading::Costs<T>& target_costs) const {
        if (pool_fields & Tag) target_pool.tag = pool.tag;
        if (pool_fields & InitialLiquidity) target_pool.initial_liq = pool.initial_liq;
        if (pool_fields & A) target_pool.A = pool.A;
        if (pool_fields & Gamma) target_pool.gamma = pool.gamma;
        if (pool_fields & MidFee) target_pool.mid_fee = pool.mid_fee;
        if (pool_fields & OutFee) target_pool.out_fee = pool.out_fee;
        if (pool_fields & FeeGamma) target_pool.fee_gamma = pool.fee_gamma;
        if (pool_fields & AdjustmentStepMin) target_pool.adjustment_step_min = pool.adjustment_step_min;
        if (pool_fields & AdjustmentStepMax) target_pool.adjustment_step_max = pool.adjustment_step_max;
        if (pool_fields & MaTime) target_pool.ma_time = pool.ma_time;
        if (pool_fields & ReservedProfitFraction) target_pool.reserved_profit_fraction = pool.reserved_profit_fraction;
        if (pool_fields & AdminFee) target_pool.admin_fee = pool.admin_fee;
        if (pool_fields & Policy) {
            target_pool.policy_kind = pool.policy_kind;
            target_pool.policy_config = pool.policy_config;
        }
        if (pool_fields & InitialPrice) target_pool.initial_price = pool.initial_price;
        if (pool_fields & StartTimestamp) target_pool.start_ts = pool.start_ts;
        if (pool_fields & HistoricalState) target_pool.historical_state = pool.historical_state;
        if (pool_fields & DonationApy) target_pool.donation_apy = pool.donation_apy;
        if (pool_fields & DonationFrequency) target_pool.donation_frequency = pool.donation_frequency;
        if (pool_fields & DonationDuration) target_pool.donation_duration = pool.donation_duration;
        if (pool_fields & InitialDonationDays) target_pool.initial_donation_days = pool.initial_donation_days;
        if (pool_fields & DonationCoinsRatio) target_pool.donation_coins_ratio = pool.donation_coins_ratio;
        if (pool_fields & UserSwapSizeFrac) target_pool.user_swap_size_frac = pool.user_swap_size_frac;

        if (cost_fields & ArbFeeBps) target_costs.arb_fee_bps = costs.arb_fee_bps;
        if (cost_fields & GasCoin0) target_costs.gas_coin0 = costs.gas_coin0;
        if (cost_fields & UseVolumeCap) target_costs.use_volume_cap = costs.use_volume_cap;
        if (cost_fields & VolumeCapMult) target_costs.volume_cap_mult = costs.volume_cap_mult;
        if (cost_fields & VolumeCapIsCoin1) target_costs.volume_cap_is_coin1 = costs.volume_cap_is_coin1;
    }

    void overlay(const PoolOverride& patch) {
        patch.apply(pool, costs);
        pool_fields |= patch.pool_fields;
        cost_fields |= patch.cost_fields;
        if (patch.yb_releverage_fee.has_value()) {
            yb_releverage_fee = patch.yb_releverage_fee;
        }
    }
};

template <typename T>
PoolOverride<T> parse_pool_override(const boost::json::object& entry) {
    PoolOverride<T> out;
    boost::json::object pool_entry = entry;
    pool_entry.erase("run");
    parse_pool_entry<T>(
        pool_entry,
        out.pool,
        out.costs,
        PoolEntryUnits::CandidateHuman
    );

    const bool wrapped_pool = pool_entry.contains("pool");
    const auto& pool = wrapped_pool ? pool_entry.at("pool").as_object() : pool_entry;
    const auto has = [&](const char* key) { return pool.if_contains(key) != nullptr; };
    if (auto* tag = entry.if_contains("tag"); tag != nullptr && tag->is_string()) out.pool_fields |= PoolOverride<T>::Tag;
    if (auto* liq = pool.if_contains("initial_liquidity"); liq != nullptr &&
        liq->is_array() && liq->as_array().size() >= 2) out.pool_fields |= PoolOverride<T>::InitialLiquidity;
    if (has("A")) out.pool_fields |= PoolOverride<T>::A;
    if (has("gamma")) out.pool_fields |= PoolOverride<T>::Gamma;
    if (has("mid_fee")) out.pool_fields |= PoolOverride<T>::MidFee;
    if (has("out_fee")) out.pool_fields |= PoolOverride<T>::OutFee;
    if (has("fee_gamma")) out.pool_fields |= PoolOverride<T>::FeeGamma;
    if (has("adjustment_step_min")) out.pool_fields |= PoolOverride<T>::AdjustmentStepMin;
    if (has("adjustment_step_max")) out.pool_fields |= PoolOverride<T>::AdjustmentStepMax;
    if (has("ma_time")) out.pool_fields |= PoolOverride<T>::MaTime;
    if (has("reserved_profit_fraction")) out.pool_fields |= PoolOverride<T>::ReservedProfitFraction;
    if (has("admin_fee")) out.pool_fields |= PoolOverride<T>::AdminFee;
    if (has("policy")) out.pool_fields |= PoolOverride<T>::Policy;
    if (has("initial_price")) out.pool_fields |= PoolOverride<T>::InitialPrice;
    if (has("start_timestamp")) out.pool_fields |= PoolOverride<T>::StartTimestamp;
    if (has("historical_state")) out.pool_fields |= PoolOverride<T>::HistoricalState;
    if (has("donation_apy")) out.pool_fields |= PoolOverride<T>::DonationApy;
    if (has("donation_frequency")) out.pool_fields |= PoolOverride<T>::DonationFrequency;
    if (has("donation_duration")) out.pool_fields |= PoolOverride<T>::DonationDuration;
    if (has("initial_donation_days")) out.pool_fields |= PoolOverride<T>::InitialDonationDays;
    if (has("donation_coins_ratio")) out.pool_fields |= PoolOverride<T>::DonationCoinsRatio;
    if (has("user_swap_size_frac")) out.pool_fields |= PoolOverride<T>::UserSwapSizeFrac;

    if (auto* costs = pool_entry.if_contains("costs"); costs != nullptr) {
        const auto& co = costs->as_object();
        if (co.if_contains("arb_fee_bps")) out.cost_fields |= PoolOverride<T>::ArbFeeBps;
        if (co.if_contains("gas_coin0")) out.cost_fields |= PoolOverride<T>::GasCoin0;
        if (co.if_contains("use_volume_cap")) out.cost_fields |= PoolOverride<T>::UseVolumeCap;
        if (co.if_contains("volume_cap_mult")) out.cost_fields |= PoolOverride<T>::VolumeCapMult;
        if (co.if_contains("volume_cap_is_coin_1")) out.cost_fields |= PoolOverride<T>::VolumeCapIsCoin1;
    }
    if (auto* run = entry.if_contains("run"); run != nullptr) {
        if (!run->is_object()) {
            throw std::runtime_error("candidate run override must be an object");
        }
        const auto& ro = run->as_object();
        reject_unknown_fields(ro, {"yb_releverage_fee"}, "candidate run override");
        if (auto* fee = ro.if_contains("yb_releverage_fee"); fee != nullptr) {
            const T value = parse_plain_real<T>(*fee);
            if (value < T(0) || value > T(1)) {
                throw std::runtime_error(
                    "candidate run override yb_releverage_fee must be in [0, 1]"
                );
            }
            out.yb_releverage_fee = value;
        }
    }
    return out;
}

} // namespace pools
} // namespace arb
