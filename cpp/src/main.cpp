// curve-fx-arb-harness - Unified evaluator executable (arb_evaluator_ld)
//
// Modes:
//   --identity-json  : Emit evaluator identity and protocol capabilities to stdout and exit 0.
//   --describe-json  : Emit executable-bound build and lowering schema to stdout and exit 0.
//   serve            : Persistent NDJSON server implementing protocol curve_fx_eval over stdin/stdout.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/json.hpp>

#include "core/common.hpp"
#include "core/json_utils.hpp"
#include "curve_fx_evaluator/compiled_grid.hpp"
#include "curve_fx_evaluator/compiled_policy_identity.hpp"
#include "curve_fx_evaluator/evaluator.hpp"
#include "curve_fx_evaluator/parameter_schema.hpp"
#include "pools/pool_config_parse.hpp"
#include "pools/twocrypto_fx/policy_descriptor.hpp"

namespace json = boost::json;
namespace fs = std::filesystem;

#if defined(ARB_MODE_LD) && defined(ARB_MODE_F64)
#error "select exactly one evaluator arithmetic mode"
#elif defined(ARB_MODE_LD)
using RealT = long double;
static constexpr const char* NUMERIC_MODE_NAME = "longdouble";
static constexpr const char* REAL_TYPE_NAME = "long double";
#elif defined(ARB_MODE_F64)
using RealT = double;
static constexpr const char* NUMERIC_MODE_NAME = "double";
static constexpr const char* REAL_TYPE_NAME = "double";
#else
#error "evaluator target must define ARB_MODE_LD or ARB_MODE_F64"
#endif

#ifndef BUILD_TARGET_NAME
#define BUILD_TARGET_NAME "arb_evaluator_ld"
#endif
#ifndef BUILD_TYPE_NAME
#define BUILD_TYPE_NAME "unknown"
#endif

static_assert(
    std::numeric_limits<double>::is_iec559 &&
        std::numeric_limits<double>::digits == 53 &&
        std::numeric_limits<double>::max_exponent == 1024,
    "curve_fx_eval requires IEEE-754 binary64 wire inputs"
);

#ifdef TWOCRYPTO_POLICY_HEADER
using SelectedPolicy = arb::pools::twocrypto_fx::ChallengeFeePolicy<RealT>;
static constexpr std::size_t SELECTED_POLICY_PARAM_COUNT =
    SelectedPolicy::DESCRIPTOR.size();
static constexpr std::string_view SELECTED_POLICY_ID =
    SelectedPolicy::DESCRIPTOR.name;
static_assert(
    SELECTED_POLICY_PARAM_COUNT <=
        arb::pools::twocrypto_fx::PolicyConfig<RealT>::MAX_POLICY_PARAMS,
    "compiled policy parameter count exceeds the pool ABI capacity"
);

constexpr bool selected_policy_order_is_canonical() {
    for (std::size_t i = 0; i < SELECTED_POLICY_PARAM_COUNT; ++i) {
        if (SelectedPolicy::DESCRIPTOR.parameters[i].order != i) return false;
    }
    return true;
}
static_assert(selected_policy_order_is_canonical(),
    "compiled policy descriptor order must be dense and canonical");

static_assert(
    std::string_view(SelectedPolicy::NAME) == SELECTED_POLICY_ID &&
        SELECTED_POLICY_ID == std::string_view(curve_fx::identity::POLICY_ID),
    "POLICY_ID must equal ChallengeFeePolicy::NAME"
);
#else
static constexpr std::size_t SELECTED_POLICY_PARAM_COUNT = 0;
static constexpr std::string_view SELECTED_POLICY_ID =
    curve_fx::identity::POLICY_ID;
#endif

namespace {

static constexpr size_t MAX_FRAME_BYTES = 4 * 1024 * 1024; // 4 MiB
static constexpr size_t MAX_CANDIDATES_PER_BATCH = 4 * 1024;
static constexpr size_t MAX_METRIC_VALUES_PER_BATCH = 128 * 1024;
static constexpr size_t MAX_MATERIALIZED_BATCH_BYTES = 64 * 1024 * 1024;

static const std::vector<std::string> CANONICAL_METRIC_FIELDS = {
    "vp", "lp_xcp_profit", "apy", "apy_net", "apy_net_gm",
    "apy_net_robust_90d", "avg_rel_price_diff", "max_rel_price_diff",
    "max_7d_rel_price_diff", "final_rel_price_diff", "detach_energy_ungated",
    "avg_imbalance",
    "tw_avg_pool_fee", "min_pool_fee", "max_pool_fee",
    "tw_real_slippage_1pct", "tw_real_slippage_5pct",
    "tw_real_slippage_10pct", "trades", "n_rebalances",
    "arb_guarded_loss_coin0", "yb_apy", "yb_apy_gm", "yb_final_growth", "yb_fee",
    "yb_releverage_trades", "yb_gm_windows", "yb_gm_floored_windows", "yb_gm_floor_share",
    "elapsed_ms", "total_notional_coin0", "lp_fee_coin0", "arb_pnl_coin0",
    "fee_capture_rate", "donations", "donation_coin0_total", "tvl_growth"
};

static const std::unordered_set<std::string> GRID_CORE_METRIC_FIELDS = {
    "vp", "lp_xcp_profit", "apy", "apy_net", "apy_net_robust_90d",
    "avg_rel_price_diff", "max_rel_price_diff", "max_7d_rel_price_diff",
    "final_rel_price_diff", "trades", "n_rebalances",
    "arb_guarded_loss_coin0", "elapsed_ms", "fee_capture_rate",
    "donations", "donation_coin0_total", "tvl_growth", "avg_imbalance",
    "detach_energy_ungated",
};

const json::object& canonical_metric_schema() {
    static const json::object schema = [] {
        json::array fields;
        for (const auto& field : CANONICAL_METRIC_FIELDS) {
            fields.push_back(json::value(field));
        }
        return json::object{
            {"metric_schema", "twocrypto-summary-v1"},
            {"metric_fields", std::move(fields)},
        };
    }();
    return schema;
}

json::value normalize_pool_override_identity_value(
    const json::value& value,
    std::string_view field
) {
    if (value.is_object()) {
        json::object normalized;
        for (const auto& item : value.as_object()) {
            normalized[item.key()] = normalize_pool_override_identity_value(
                item.value(), item.key());
        }
        return normalized;
    }
    if (value.is_array()) {
        json::array normalized;
        for (const auto& item : value.as_array()) {
            normalized.push_back(normalize_pool_override_identity_value(
                item, field));
        }
        return normalized;
    }
    const bool textual =
        field == "tag" || field == "kind" || field == "policy";
    if (!textual && arb::pools::is_number_or_string(value)) {
        return json::value(arb::canonical_float_string(
            arb::parse_input_double(value)));
    }
    return value;
}

json::object normalize_pool_override_identity(const json::object& value) {
    return normalize_pool_override_identity_value(value, "").as_object();
}

std::optional<std::string> unknown_field(
    const json::object& object,
    std::initializer_list<std::string_view> allowed
);

json::object make_evaluator_identity() {
    json::object id;
    id["harness_version"] = curve_fx::identity::HARNESS_VERSION;
    id["pool_version"] = curve_fx::identity::POOL_VERSION;
    id["policy_id"] = std::string(SELECTED_POLICY_ID);
    id["policy_abi"] = curve_fx::identity::POLICY_ABI;
    id["policy_parameter_count"] = SELECTED_POLICY_PARAM_COUNT;
    id["numeric_mode"] = NUMERIC_MODE_NAME;
    id["real_type"] = REAL_TYPE_NAME;
    id["compiler"] = curve_fx::identity::COMPILER_ID;
    id["build_target"] = BUILD_TARGET_NAME;
    id["ipo_enabled"] = curve_fx::identity::ENABLE_IPO;
    id["native_tuning"] = curve_fx::identity::NATIVE_TUNING;
    return id;
}

json::value source_dirty_value(std::string_view value) {
    if (value == "true" || value == "TRUE" || value == "1") return true;
    if (value == "false" || value == "FALSE" || value == "0") return false;
    return nullptr;
}

json::object make_parameter_schema() {
    json::array parameters;
#ifdef TWOCRYPTO_POLICY_HEADER
    for (const auto& descriptor : SelectedPolicy::DESCRIPTOR.parameters) {
        json::object value;
        value["name"] = "policy." + std::string(descriptor.name);
        value["lowering_path"] =
            "evaluate_batch.candidates[].policy_params[" +
            std::to_string(descriptor.order) + "]";
        value["order"] = descriptor.order;
        value["type"] = "real";
        value["unit"] = std::string(descriptor.unit);
        value["wire_representation"] = "finite_binary64";
        value["classification"] = "candidate";
        value["default"] = static_cast<double>(descriptor.default_value);
        value["minimum"] = static_cast<double>(descriptor.minimum);
        value["maximum"] = static_cast<double>(descriptor.maximum);
        value["quantum"] = static_cast<double>(descriptor.quantum);
        parameters.push_back(std::move(value));
    }
#endif
    for (const auto& descriptor : curve_fx::evaluator::STATIC_PARAMETERS) {
        json::object value;
        value["name"] = std::string(descriptor.name);
        value["lowering_path"] = std::string(descriptor.lowering_path);
        value["type"] = std::string(descriptor.type);
        value["unit"] = std::string(descriptor.unit);
        value["wire_representation"] = std::string(descriptor.wire);
        value["classification"] = std::string(descriptor.classification);
        if (!descriptor.default_json.empty()) {
            value["default"] = json::parse(descriptor.default_json);
        }
        if (!descriptor.choices_json.empty()) {
            value["choices"] = json::parse(descriptor.choices_json);
        }
        parameters.push_back(std::move(value));
    }
    json::object schema;
    schema["schema_version"] = "curve_fx_parameter_schema_v1";
    schema["parameters"] = std::move(parameters);
    return schema;
}

json::object make_description() {
    json::object info;
    info["schema_version"] = "curve_fx_evaluator_description_v1";

    json::object harness;
    harness["version"] = curve_fx::identity::HARNESS_VERSION;
    harness["revision"] = curve_fx::identity::HARNESS_GIT_REVISION;
    harness["dirty"] = source_dirty_value(
        curve_fx::identity::HARNESS_GIT_DIRTY);
    info["harness"] = std::move(harness);

    json::object pool;
    pool["version"] = curve_fx::identity::POOL_VERSION;
    info["pool"] = std::move(pool);

    json::object policy;
    policy["id"] = std::string(SELECTED_POLICY_ID);
    policy["abi"] = curve_fx::identity::POLICY_ABI;
    policy["parameter_count"] = SELECTED_POLICY_PARAM_COUNT;
#ifdef TWOCRYPTO_POLICY_HEADER
    policy["descriptor_abi_version"] =
        arb::pools::twocrypto_fx::POLICY_DESCRIPTOR_ABI_VERSION;
#endif
    info["policy"] = std::move(policy);

    json::object build;
    build["type"] = BUILD_TYPE_NAME;
    build["compiler"] = curve_fx::identity::COMPILER_ID;
    build["target"] = BUILD_TARGET_NAME;
    build["numeric_mode"] = NUMERIC_MODE_NAME;
    build["real_type"] = REAL_TYPE_NAME;
    build["real_digits"] = std::numeric_limits<RealT>::digits;
    build["real_max_digits10"] = std::numeric_limits<RealT>::max_digits10;
    build["wire_real_type"] = "IEEE-754 binary64";
    build["wire_real_digits"] = std::numeric_limits<double>::digits;
    build["ipo_enabled"] = curve_fx::identity::ENABLE_IPO;
    build["native_tuning"] = curve_fx::identity::NATIVE_TUNING;
    info["build"] = std::move(build);

    json::object schema = make_parameter_schema();
    const std::string schema_canonical_json = arb::canonical_json(schema);
    info["parameter_schema_canonical_json"] = schema_canonical_json;
    info["parameter_schema"] = std::move(schema);
    return info;
}

json::object make_hello_frame() {
    json::object hello;
    hello["protocol"] = "curve_fx_eval";
    hello["type"] = "hello";
    hello["evaluator_identity"] = make_evaluator_identity();

    json::array caps;
    caps.push_back("summary");
    caps.push_back("full_trace");
    caps.push_back("atomic_sidecars");
    caps.push_back("registered_grid_ranges");
    hello["capabilities"] = caps;

    json::array yb_modes;
    yb_modes.push_back("off");
    yb_modes.push_back("active_2l");
    yb_modes.push_back("reference_2l");
    hello["yb_modes"] = yb_modes;

    const auto& metric_schema = canonical_metric_schema();
    hello["metric_schema"] = metric_schema.at("metric_schema");
    hello["metric_fields"] = metric_schema.at("metric_fields");

    json::object limits;
    limits["max_frame_bytes"] = MAX_FRAME_BYTES;
    limits["max_candidates_per_batch"] = MAX_CANDIDATES_PER_BATCH;
    limits["max_metric_values_per_batch"] = MAX_METRIC_VALUES_PER_BATCH;
    limits["max_materialized_batch_bytes"] = MAX_MATERIALIZED_BATCH_BYTES;
    limits["max_inflight_batches"] = 1;
    hello["limits"] = limits;

    return hello;
}

json::object make_error_frame(
    const std::string& req_id,
    const std::string& scope,
    const std::string& error_code,
    const std::string& message,
    const json::object& details = json::object{}
) {
    json::object err;
    err["protocol"] = "curve_fx_eval";
    err["type"] = "error";
    err["request_id"] = req_id;
    err["scope"] = scope;
    err["error_code"] = error_code;
    err["message"] = message;
    if (!details.empty()) {
        err["details"] = details;
    }
    return err;
}

void write_frame(std::ostream& os, const json::object& obj) {
    arb::write_json_plain(os, obj);
    os << '\n' << std::flush;
}

bool is_safe_identifier(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) {
            return false;
        }
    }
    return true;
}

std::optional<std::string> unknown_field(
    const json::object& object,
    std::initializer_list<std::string_view> allowed
) {
    for (const auto& item : object) {
        const std::string_view key(item.key().data(), item.key().size());
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
            return std::string(key);
        }
    }
    return std::nullopt;
}

bool write_atomic_file(const fs::path& p, const std::string& content) {
    if (p.has_parent_path()) {
        fs::create_directories(p.parent_path());
    }
    fs::path tmp_p = p;
    tmp_p += ".tmp";

    {
        std::ofstream ofs(tmp_p, std::ios::binary | std::ios::trunc);
        if (!ofs) return false;
        ofs.write(content.data(), content.size());
        ofs.flush();
    }

    std::error_code ec;
    fs::rename(tmp_p, p, ec);
    if (ec) {
        std::error_code cleanup_error;
        fs::remove(tmp_p, cleanup_error);
    }
    return !ec;
}

} // namespace

namespace curve_fx {
namespace server {

struct ActiveSession {
    std::string session_id;
    std::shared_ptr<curve_fx::evaluator::ScenarioStore<RealT>> store;
    curve_fx::evaluator::SessionConfig<RealT> config;
    std::string grid_id;
    std::optional<curve_fx::evaluator::CompiledGrid<RealT>> grid;
};

class EvaluatorServer {
public:
    void run() {
        std::cerr << "[evaluator] Worker count: "
                  << curve_fx::evaluator::configured_worker_count() << "\n";
        // Emit initial hello frame
        write_frame(std::cout, make_hello_frame());

        std::string line;
        while (std::getline(std::cin, line)) {
            if (line.empty()) continue;

            if (line.size() > MAX_FRAME_BYTES) {
                write_frame(
                    std::cout,
                    make_error_frame("unknown", "protocol", "FRAME_SIZE_EXCEEDED",
                        "Frame size " + std::to_string(line.size()) + " exceeds limit of " + std::to_string(MAX_FRAME_BYTES) + " bytes")
                );
                continue;
            }

            json::value req_val;
            try {
                req_val = json::parse(line);
            } catch (const std::exception& e) {
                write_frame(
                    std::cout,
                    make_error_frame("unknown", "protocol", "JSON_PARSE_ERROR", e.what())
                );
                continue;
            }

            if (!req_val.is_object()) {
                write_frame(
                    std::cout,
                    make_error_frame("unknown", "protocol", "FRAME_NOT_OBJECT", "Frame must be a JSON object")
                );
                continue;
            }

            const auto& req = req_val.as_object();
            const std::string protocol = arb::get_string_opt(req, "protocol", "");
            const std::string type = arb::get_string_opt(req, "type", "");
            const std::string req_id = arb::get_string_opt(req, "request_id", "req-unknown");

            if (protocol != "curve_fx_eval") {
                write_frame(
                    std::cout,
                    make_error_frame(req_id, "protocol", "PROTOCOL_MISMATCH",
                        "protocol must be 'curve_fx_eval'")
                );
                continue;
            }

            if (type == "open_session") {
                handle_open_session(req, req_id);
            } else if (type == "register_grid") {
                handle_register_grid(req, req_id);
            } else if (type == "evaluate_batch") {
                handle_evaluate_batch(req, req_id);
            } else if (type == "close_session") {
                handle_close_session(req, req_id);
            } else if (type == "shutdown") {
                if (const auto field = unknown_field(
                        req, {"protocol", "type", "request_id"})) {
                    write_frame(std::cout, make_error_frame(
                        req_id, "protocol", "UNKNOWN_FIELD",
                        "shutdown contains unknown field: " + *field));
                    continue;
                }
                std::cerr << "[evaluator] Shutdown requested via protocol.\n";
                break;
            } else {
                write_frame(
                    std::cout,
                    make_error_frame(req_id, "protocol", "UNKNOWN_MESSAGE_TYPE", "Unknown message type: " + type)
                );
            }
        }
    }

private:
    std::optional<ActiveSession> session_;
    bool session_ever_opened_{false};

    void handle_open_session(const json::object& req, const std::string& req_id) {
        if (const auto field = unknown_field(req, {
                "protocol", "type", "request_id", "session_id",
                "template_path", "scenario_id", "market_path", "price_feed_path",
                "pool_index", "n_candles", "start_time",
                "end_time", "candle_filter", "min_swap", "max_swap",
                "dustswap_freq_s", "user_swap_freq_s",
                "user_swap_size_frac", "user_swap_thresh",
                "enable_slippage_probes", "yb_releverage_fee",
                "yb_cash_multiplier", "yb_mode", "event_cursor",
                "metric_profile"
            })) {
            write_frame(std::cout, make_error_frame(
                req_id, "protocol", "UNKNOWN_FIELD",
                "open_session contains unknown field: " + *field));
            return;
        }
        if (session_ever_opened_) {
            write_frame(std::cout, make_error_frame(req_id, "session", "SESSION_ALREADY_INITIALIZED",
                "Evaluator session admission is one-shot per process lifetime to ensure immutable scenario loading"));
            return;
        }

        const std::string session_id = arb::get_string_opt(req, "session_id", "");
        if (session_id.empty()) {
            write_frame(std::cout, make_error_frame(req_id, "session", "INVALID_ARGUMENT", "session_id is required"));
            return;
        }

        const std::string tpl_path = arb::get_string_opt(req, "template_path", "");
        const std::string scenario_id = arb::get_string_opt(req, "scenario_id", "");
        const std::string market_path = arb::get_string_opt(req, "market_path", "");
        const std::string price_feed_path = arb::get_string_opt(req, "price_feed_path", "");

        if (tpl_path.empty() || scenario_id.empty() || market_path.empty()) {
            write_frame(std::cout, make_error_frame(
                req_id, "session", "INVALID_ARGUMENT",
                "template_path, scenario_id, and market_path are required"));
            return;
        }

        if (!fs::exists(tpl_path) || fs::is_directory(tpl_path)) {
            write_frame(std::cout, make_error_frame(req_id, "session", "FILE_NOT_FOUND", "Template file not found: " + tpl_path));
            return;
        }
        if (!fs::exists(market_path) || fs::is_directory(market_path)) {
            write_frame(std::cout, make_error_frame(req_id, "session", "FILE_NOT_FOUND", "Market file not found: " + market_path));
            return;
        }
        if (!price_feed_path.empty() &&
            (!fs::exists(price_feed_path) || fs::is_directory(price_feed_path))) {
            write_frame(std::cout, make_error_frame(req_id, "session", "FILE_NOT_FOUND", "Price-feed file not found: " + price_feed_path));
            return;
        }
        size_t pool_index = 0;
        size_t max_candles = 0;
        uint64_t start_ts = 0;
        uint64_t end_ts = 0;
        uint64_t dustswap_freq_s = 0;
        uint64_t user_swap_freq_s = 0;
        const auto parse_integer = [&](const char* key, auto fallback, auto& out) {
            if (arb::parse_bounded_uint_field(req, key, fallback, out)) return true;
            write_frame(std::cout, make_error_frame(
                req_id, "session", "INVALID_ARGUMENT",
                std::string(key) + " must be a non-negative integer in range"));
            return false;
        };
        if (!parse_integer("pool_index", size_t{0}, pool_index) ||
            !parse_integer("n_candles", size_t{0}, max_candles) ||
            !parse_integer("start_time", uint64_t{0}, start_ts) ||
            !parse_integer("end_time", uint64_t{0}, end_ts) ||
            !parse_integer("dustswap_freq_s", uint64_t{3600}, dustswap_freq_s) ||
            !parse_integer("user_swap_freq_s", uint64_t{0}, user_swap_freq_s)) {
            return;
        }

        try {
            auto store = std::make_shared<curve_fx::evaluator::ScenarioStore<RealT>>();
            curve_fx::evaluator::ScenarioLoadOptions opts{};
            opts.pool_index = pool_index;
            opts.max_candles = max_candles;
            opts.start_ts = start_ts;
            opts.end_ts = end_ts;
            opts.candle_filter_pct = arb::get_double_opt(req, "candle_filter", 0.0);

            std::cerr << "[evaluator] Loading scenario '" << scenario_id
                      << "' from " << market_path << " with template: " << tpl_path << "\n";
            store->load(tpl_path, scenario_id, market_path, price_feed_path, opts);

            curve_fx::evaluator::SessionConfig<RealT> cfg{};
            cfg.min_swap_frac = static_cast<RealT>(arb::get_double_opt(req, "min_swap", 1e-6));
            cfg.max_swap_frac = static_cast<RealT>(arb::get_double_opt(req, "max_swap", 1.0));
            cfg.start_ts = start_ts;
            cfg.dustswap_freq_s = dustswap_freq_s;
            cfg.user_swap_freq_s = user_swap_freq_s;
            cfg.user_swap_size_frac = static_cast<RealT>(arb::get_double_opt(req, "user_swap_size_frac", 0.01));
            cfg.user_swap_thresh = static_cast<RealT>(arb::get_double_opt(req, "user_swap_thresh", 0.05));
            cfg.enable_slippage_probes =
                req.if_contains("enable_slippage_probes") &&
                req.at("enable_slippage_probes").as_bool();
            std::string event_cursor = "scalar";
            if (req.if_contains("event_cursor")) {
                if (!req.at("event_cursor").is_string()) {
                    write_frame(std::cout, make_error_frame(
                        req_id, "protocol", "INVALID_ARGUMENT",
                        "event_cursor must be a string"));
                    return;
                }
                event_cursor = std::string(req.at("event_cursor").as_string());
                if (event_cursor != "scalar" && event_cursor != "exact_skip") {
                    write_frame(std::cout, make_error_frame(
                        req_id, "protocol", "INVALID_ARGUMENT",
                        "event_cursor must be 'scalar' or 'exact_skip'"));
                    return;
                }
            }
            cfg.event_cursor = event_cursor;
            std::string metric_profile = "full_summary";
            if (req.if_contains("metric_profile")) {
                if (!req.at("metric_profile").is_string()) {
                    write_frame(std::cout, make_error_frame(
                        req_id, "protocol", "INVALID_ARGUMENT",
                        "metric_profile must be a string"));
                    return;
                }
                metric_profile = std::string(
                    req.at("metric_profile").as_string()
                );
                if (
                    metric_profile != "full_summary" &&
                    metric_profile != "grid_core"
                ) {
                    write_frame(std::cout, make_error_frame(
                        req_id, "protocol", "INVALID_ARGUMENT",
                        "metric_profile must be 'full_summary' or 'grid_core'"));
                    return;
                }
            }
            cfg.metric_profile = metric_profile;
            if (
                cfg.event_cursor == "exact_skip" &&
                cfg.metric_profile != "grid_core"
            ) {
                write_frame(std::cout, make_error_frame(
                    req_id, "session", "INVALID_EVENT_CURSOR",
                    "exact_skip requires metric_profile='grid_core'"));
                return;
            }
            std::string yb_mode = "off";
            if (req.if_contains("yb_mode")) {
                if (!req.at("yb_mode").is_string()) {
                    write_frame(std::cout, make_error_frame(
                        req_id, "protocol", "INVALID_ARGUMENT",
                        "yb_mode must be a string"));
                    return;
                }
                yb_mode = std::string(req.at("yb_mode").as_string());
                if (yb_mode != "off" && yb_mode != "active_2l" &&
                    yb_mode != "reference_2l") {
                    write_frame(std::cout, make_error_frame(
                        req_id, "protocol", "INVALID_ARGUMENT",
                        "yb_mode must be one of 'off', 'active_2l', 'reference_2l'"));
                    return;
                }
            }
            cfg.yb_mode = yb_mode;
            if (
                cfg.metric_profile == "grid_core" &&
                (cfg.yb_mode != "off" || cfg.enable_slippage_probes)
            ) {
                write_frame(std::cout, make_error_frame(
                    req_id, "session", "INVALID_METRIC_PROFILE",
                    "grid_core requires yb_mode='off' and slippage disabled"));
                return;
            }
            cfg.yb_releverage_fee = static_cast<RealT>(arb::get_double_opt(req, "yb_releverage_fee", 0.012));
            cfg.yb_cash_multiplier = static_cast<RealT>(
                arb::get_double_opt(req, "yb_cash_multiplier", 1.0));

            ActiveSession sess;
            sess.session_id = session_id;
            sess.store = store;
            sess.config = cfg;
            session_ = std::move(sess);
            session_ever_opened_ = true;

            json::object resp;
            resp["protocol"] = "curve_fx_eval";
            resp["type"] = "session_ready";
            resp["request_id"] = req_id;
            resp["session_id"] = session_id;

            json::array scen_arr;
            for (const auto& sc : store->scenarios()) {
                json::object s;
                s["id"] = sc.id;
                s["events_count"] = sc.events.size();
                s["candles_count"] = sc.candles.size();
                s["start_ts"] = sc.start_ts;
                s["end_ts"] = sc.candles.empty() ? 0 : sc.candles.back().ts;
                scen_arr.push_back(s);
            }
            resp["scenarios"] = scen_arr;
            write_frame(std::cout, resp);
            std::cerr << "[evaluator] Session '" << session_id << "' initialized successfully with "
                      << store->scenarios().size() << " scenarios.\n";

        } catch (const std::exception& e) {
            write_frame(std::cout, make_error_frame(req_id, "session", "SESSION_INIT_FAILED", e.what()));
        }
    }

    void handle_register_grid(const json::object& req, const std::string& req_id) {
        if (const auto field = unknown_field(req, {
                "protocol", "type", "request_id", "session_id", "grid_id",
                "candidate_defaults", "axes", "axis_order", "shape"
            })) {
            write_frame(std::cout, make_error_frame(
                req_id, "protocol", "UNKNOWN_FIELD",
                "register_grid contains unknown field: " + *field));
            return;
        }
        if (!session_.has_value()) {
            write_frame(std::cout, make_error_frame(
                req_id, "session", "NO_ACTIVE_SESSION",
                "No session is open. Call open_session first."));
            return;
        }
        const std::string session_id = arb::get_string_opt(req, "session_id", "");
        if (session_id != session_->session_id) {
            write_frame(std::cout, make_error_frame(
                req_id, "session", "SESSION_MISMATCH",
                "register_grid session_id does not match the active session"));
            return;
        }
        if (session_->grid.has_value()) {
            write_frame(std::cout, make_error_frame(
                req_id, "session", "GRID_ALREADY_REGISTERED",
                "one grid may be registered per evaluator session"));
            return;
        }
        const std::string grid_id = arb::get_string_opt(req, "grid_id", "");
        if (!is_safe_identifier(grid_id)) {
            write_frame(std::cout, make_error_frame(
                req_id, "candidate", "INVALID_GRID_ID",
                "grid_id must be a safe non-empty identifier"));
            return;
        }
        json::object grid;
        for (const char* field : {
                "candidate_defaults", "axes", "axis_order", "shape"
            }) {
            const auto* value = req.if_contains(field);
            if (value == nullptr) {
                write_frame(std::cout, make_error_frame(
                    req_id, "candidate", "INVALID_GRID",
                    std::string("register_grid requires ") + field));
                return;
            }
            grid[field] = *value;
        }
        std::string error;
        auto compiled = curve_fx::evaluator::CompiledGrid<RealT>::compile(
            grid, SELECTED_POLICY_PARAM_COUNT, error);
        if (!compiled.has_value()) {
            write_frame(std::cout, make_error_frame(
                req_id, "candidate", "INVALID_GRID", error));
            return;
        }
        const uint64_t candidate_count = compiled->size();
        session_->grid_id = grid_id;
        session_->grid = std::move(*compiled);

        json::object response;
        response["protocol"] = "curve_fx_eval";
        response["type"] = "grid_ready";
        response["request_id"] = req_id;
        response["session_id"] = session_id;
        response["grid_id"] = grid_id;
        response["candidate_count"] = candidate_count;
        write_frame(std::cout, response);
    }

    void handle_evaluate_batch(const json::object& req, const std::string& req_id) {
        if (const auto field = unknown_field(req, {
                "protocol", "type", "request_id", "session_id",
                "metric_projection", "metric_fields", "metrics_format",
                "observation", "candidates", "grid_id", "ranges"
            })) {
            write_frame(std::cout, make_error_frame(
                req_id, "protocol", "UNKNOWN_FIELD",
                "evaluate_batch contains unknown field: " + *field));
            return;
        }
        if (!session_.has_value()) {
            write_frame(std::cout, make_error_frame(req_id, "session", "NO_ACTIVE_SESSION", "No session is open. Call open_session first."));
            return;
        }

        const std::string session_id = arb::get_string_opt(req, "session_id", "");
        if (session_id != session_->session_id) {
            write_frame(std::cout, make_error_frame(req_id, "session", "SESSION_MISMATCH",
                "Request session_id '" + session_id + "' does not match active session '" + session_->session_id + "'"));
            return;
        }

        // metric_projection is required on the wire; do not silently choose a
        // projection when a client omits it.
        const auto* metric_projection_value = req.if_contains("metric_projection");
        if (metric_projection_value == nullptr) {
            write_frame(std::cout, make_error_frame(
                req_id, "protocol", "MISSING_REQUIRED_FIELD",
                "evaluate_batch requires metric_projection ('summary' or 'full')"));
            return;
        }
        if (!metric_projection_value->is_string()) {
            write_frame(std::cout, make_error_frame(
                req_id, "protocol", "INVALID_METRIC_PROJECTION",
                "metric_projection must be 'summary' or 'full'"));
            return;
        }
        const std::string metric_proj_str = metric_projection_value->as_string().c_str();
        if (metric_proj_str != "summary" && metric_proj_str != "full") {
            write_frame(std::cout, make_error_frame(req_id, "protocol", "INVALID_METRIC_PROJECTION",
                "metric_projection must be 'summary' or 'full'"));
            return;
        }
        const bool full_metric_projection = (metric_proj_str == "full");
        std::string metrics_format = "object";
        if (const auto* value = req.if_contains("metrics_format")) {
            if (!value->is_string()) {
                write_frame(std::cout, make_error_frame(
                    req_id, "protocol", "INVALID_METRICS_FORMAT",
                    "metrics_format must be a string"));
                return;
            }
            metrics_format = value->as_string().c_str();
        }
        if (metrics_format != "object" && metrics_format != "array") {
            write_frame(std::cout, make_error_frame(
                req_id, "protocol", "INVALID_METRICS_FORMAT",
                "metrics_format must be 'object' or 'array'"));
            return;
        }
        if (metrics_format == "array" &&
            req.if_contains("metric_fields") == nullptr) {
            write_frame(std::cout, make_error_frame(
                req_id, "protocol", "INVALID_METRIC_FIELDS",
                "array metrics require metric_fields"));
            return;
        }
        std::vector<std::string> metric_fields = CANONICAL_METRIC_FIELDS;
        if (const auto* value = req.if_contains("metric_fields")) {
            if (!value->is_array() || value->as_array().empty()) {
                write_frame(std::cout, make_error_frame(
                    req_id, "protocol", "INVALID_METRIC_FIELDS",
                    "metric_fields must be a non-empty array"));
                return;
            }
            metric_fields.clear();
            std::unordered_set<std::string> seen;
            for (const auto& item : value->as_array()) {
                if (!item.is_string()) {
                    write_frame(std::cout, make_error_frame(
                        req_id, "protocol", "INVALID_METRIC_FIELDS",
                        "metric_fields entries must be strings"));
                    return;
                }
                const std::string name(item.as_string().c_str());
                if (std::find(
                        CANONICAL_METRIC_FIELDS.begin(),
                        CANONICAL_METRIC_FIELDS.end(),
                        name
                    ) == CANONICAL_METRIC_FIELDS.end() || !seen.insert(name).second) {
                    write_frame(std::cout, make_error_frame(
                        req_id, "protocol", "INVALID_METRIC_FIELDS",
                        "metric_fields contains an unknown or duplicate field: " + name));
                    return;
                }
                metric_fields.push_back(name);
            }
        }
        if (session_->config.metric_profile == "grid_core") {
            for (const auto& name : metric_fields) {
                if (GRID_CORE_METRIC_FIELDS.count(name) == 0) {
                    write_frame(std::cout, make_error_frame(
                        req_id, "protocol", "INVALID_METRIC_PROFILE",
                        "grid_core does not provide metric field: " + name));
                    return;
                }
            }
        }
        const bool has_candidates = req.if_contains("candidates") != nullptr;
        const bool has_grid_id = req.if_contains("grid_id") != nullptr;
        const bool has_ranges = req.if_contains("ranges") != nullptr;
        if (has_candidates == (has_grid_id || has_ranges) ||
            has_grid_id != has_ranges) {
            write_frame(std::cout, make_error_frame(
                req_id, "candidate", "INVALID_ARGUMENT",
                "provide either candidates or a registered grid_id with ranges"));
            return;
        }

        const json::array* candidate_array = nullptr;
        std::vector<curve_fx::evaluator::EvaluationCandidate<RealT>> grid_candidates;
        if (has_candidates) {
            if (!req.at("candidates").is_array()) {
                write_frame(std::cout, make_error_frame(
                    req_id, "candidate", "INVALID_ARGUMENT",
                    "candidates must be an array"));
                return;
            }
            if (req.at("candidates").as_array().size() >
                MAX_CANDIDATES_PER_BATCH) {
                write_frame(std::cout, make_error_frame(
                    req_id, "candidate", "BATCH_TOO_LARGE",
                    "candidate batch exceeds the candidate limit"));
                return;
            }
            candidate_array = &req.at("candidates").as_array();
        } else {
            if (!req.at("grid_id").is_string() || !req.at("ranges").is_array()) {
                write_frame(std::cout, make_error_frame(
                    req_id, "candidate", "INVALID_ARGUMENT",
                    "grid_id must be a string and ranges must be an array"));
                return;
            }
            const std::string grid_id(req.at("grid_id").as_string().c_str());
            if (!session_->grid.has_value() || grid_id != session_->grid_id) {
                write_frame(std::cout, make_error_frame(
                    req_id, "session", "GRID_MISMATCH",
                    "grid_id does not match the registered grid"));
                return;
            }
            std::string error;
            if (!session_->grid->materialize_ranges(
                    req.at("ranges").as_array(), MAX_CANDIDATES_PER_BATCH,
                    grid_candidates, error)) {
                write_frame(std::cout, make_error_frame(
                    req_id, "candidate", "INVALID_GRID_RANGES", error));
                return;
            }
        }

        const size_t candidate_count = has_candidates
            ? candidate_array->size()
            : grid_candidates.size();
        if (candidate_count == 0) {
            write_frame(std::cout, make_error_frame(
                req_id, "candidate", "EMPTY_BATCH",
                "candidate batch cannot be empty"));
            return;
        }
        if (metric_fields.size() >
            MAX_METRIC_VALUES_PER_BATCH / candidate_count) {
            write_frame(std::cout, make_error_frame(
                req_id, "candidate", "BATCH_TOO_LARGE",
                "candidate count times metric count exceeds the result limit"));
            return;
        }

        // Candidate, materialization, and result-cell admission guards bound
        // allocations that compact ordinal requests can trigger.

        // Observation options (trace capture)
        curve_fx::evaluator::ObservationSpec obs_spec{};
        std::string artifact_dir;
        if (req.if_contains("observation") && !req.at("observation").is_object()) {
            write_frame(std::cout, make_error_frame(
                req_id, "protocol", "INVALID_OBSERVATION",
                "observation must be an object"));
            return;
        }
        if (req.if_contains("observation")) {
            const auto& obs_obj = req.at("observation").as_object();
            if (const auto field = unknown_field(obs_obj, {
                    "kind", "trace_interval", "trace_actions", "artifact_dir"
                })) {
                write_frame(std::cout, make_error_frame(
                    req_id, "protocol", "UNKNOWN_FIELD",
                    "observation contains unknown field: " + *field));
                return;
            }
            const std::string kind_str = arb::get_string_opt(obs_obj, "kind", "summary");
            if (kind_str == "full_trace") {
                obs_spec.kind = curve_fx::evaluator::ObservationKind::FullTrace;
            } else if (kind_str == "summary") {
                obs_spec.kind = curve_fx::evaluator::ObservationKind::Summary;
            } else {
                write_frame(std::cout, make_error_frame(
                    req_id, "protocol", "INVALID_OBSERVATION",
                    "observation.kind must be 'summary' or 'full_trace'"));
                return;
            }
            size_t trace_interval = 1;
            if (!arb::parse_bounded_uint_field(
                    obs_obj, "trace_interval", size_t{1}, trace_interval) ||
                trace_interval == 0) {
                write_frame(std::cout, make_error_frame(
                    req_id, "protocol", "INVALID_ARGUMENT",
                    "observation.trace_interval must be a positive integer"));
                return;
            }
            obs_spec.trace_interval = trace_interval;
            if (obs_obj.if_contains("trace_actions") &&
                !obs_obj.at("trace_actions").is_bool()) {
                write_frame(std::cout, make_error_frame(
                    req_id, "protocol", "INVALID_OBSERVATION",
                    "observation.trace_actions must be a boolean"));
                return;
            }
            if (obs_obj.if_contains("artifact_dir") &&
                !obs_obj.at("artifact_dir").is_string()) {
                write_frame(std::cout, make_error_frame(
                    req_id, "protocol", "INVALID_OBSERVATION",
                    "observation.artifact_dir must be a string"));
                return;
            }
            obs_spec.trace_actions = obs_obj.if_contains("trace_actions") &&
                obs_obj.at("trace_actions").as_bool();
            artifact_dir = arb::get_string_opt(obs_obj, "artifact_dir", "");

            if (obs_spec.kind == curve_fx::evaluator::ObservationKind::FullTrace &&
                artifact_dir.empty()) {
                write_frame(std::cout, make_error_frame(
                    req_id, "sidecar", "ARTIFACT_DIR_REQUIRED",
                    "full_trace observation requires artifact_dir"));
                return;
            }

        }
        if (
            session_->config.metric_profile == "grid_core" &&
            obs_spec.kind != curve_fx::evaluator::ObservationKind::Summary
        ) {
            write_frame(std::cout, make_error_frame(
                req_id, "protocol", "INVALID_METRIC_PROFILE",
                "grid_core supports summary observation only"));
            return;
        }

        auto t0 = std::chrono::high_resolution_clock::now();

        std::vector<curve_fx::evaluator::EvaluationCandidate<RealT>> candidates =
            std::move(grid_candidates);
        if (has_candidates) {
            const auto& cand_arr = *candidate_array;
            candidates.reserve(cand_arr.size());
            std::unordered_set<std::string> candidate_ids;
            std::unordered_set<uint32_t> candidate_ordinals;

            for (size_t i = 0; i < cand_arr.size(); ++i) {
                if (!cand_arr[i].is_object()) {
                    write_frame(std::cout, make_error_frame(req_id, "candidate", "INVALID_CANDIDATE", "Candidate entry must be an object"));
                    return;
                }
                const auto& c_obj = cand_arr[i].as_object();
                if (const auto field = unknown_field(c_obj, {
                        "ordinal", "candidate_id", "policy_params", "pool_overrides"
                    })) {
                    write_frame(std::cout, make_error_frame(
                        req_id, "candidate", "UNKNOWN_FIELD",
                        "candidate contains unknown field: " + *field));
                    return;
                }
                curve_fx::evaluator::EvaluationCandidate<RealT> cand{};
                uint32_t ordinal = 0;
                if (!c_obj.if_contains("ordinal") ||
                    !arb::parse_bounded_uint_field(
                        c_obj, "ordinal", uint32_t{0}, ordinal)) {
                    write_frame(std::cout, make_error_frame(
                        req_id, "candidate", "INVALID_ARGUMENT",
                        "candidate ordinal must be an unsigned 32-bit integer"));
                    return;
                }
                cand.ordinal = ordinal;
                cand.candidate_id = arb::get_string_opt(c_obj, "candidate_id", "");

                if (!is_safe_identifier(cand.candidate_id)) {
                    write_frame(std::cout, make_error_frame(req_id, "candidate", "INVALID_CANDIDATE_ID",
                        "candidate_id contains unsafe path characters: " + cand.candidate_id));
                    return;
                }
                if (!candidate_ids.insert(cand.candidate_id).second ||
                    !candidate_ordinals.insert(cand.ordinal).second) {
                    write_frame(std::cout, make_error_frame(
                        req_id,
                        "candidate",
                        "DUPLICATE_CANDIDATE",
                        "candidate_id and ordinal must both be unique within a batch"
                    ));
                    return;
                }

                if (c_obj.if_contains("policy_params")) {
                    if (!c_obj.at("policy_params").is_array()) {
                        write_frame(std::cout, make_error_frame(req_id, "candidate", "INVALID_POLICY_PARAMS",
                            "policy_params must be an array"));
                        return;
                    }
                    for (const auto& p_val : c_obj.at("policy_params").as_array()) {
                        if (!p_val.is_double() && !p_val.is_int64() &&
                            !p_val.is_uint64()) {
                            write_frame(std::cout, make_error_frame(req_id, "candidate", "INVALID_POLICY_PARAMS",
                                "every policy parameter must be numeric"));
                            return;
                        }
                        const double parsed = arb::parse_input_double(p_val);
                        cand.policy_params.push_back(static_cast<RealT>(parsed));
                    }
                }

                if (cand.policy_params.size() != SELECTED_POLICY_PARAM_COUNT) {
                    write_frame(std::cout, make_error_frame(
                        req_id,
                        "candidate",
                        "POLICY_PARAM_COUNT_MISMATCH",
                        "expected " + std::to_string(SELECTED_POLICY_PARAM_COUNT) +
                            " policy parameters, got " +
                            std::to_string(cand.policy_params.size())
                    ));
                    return;
                }

                if (c_obj.if_contains("pool_overrides")) {
                    if (!c_obj.at("pool_overrides").is_object()) {
                        write_frame(std::cout, make_error_frame(req_id, "candidate", "INVALID_POOL_OVERRIDES",
                            "pool_overrides must be an object"));
                        return;
                    }
                    cand.pool_overrides = c_obj.at("pool_overrides").as_object();
                    const auto* nested_pool = cand.pool_overrides.if_contains("pool");
                    if (cand.pool_overrides.if_contains("policy") ||
                        (nested_pool != nullptr && nested_pool->is_object() &&
                         nested_pool->as_object().if_contains("policy"))) {
                        write_frame(std::cout, make_error_frame(
                            req_id, "candidate", "INVALID_POOL_OVERRIDES",
                            "candidate pool_overrides.pool.policy is prohibited; use policy_params"));
                        return;
                    }
                    try {
                        (void)normalize_pool_override_identity(cand.pool_overrides);
                    } catch (const std::exception& error) {
                        write_frame(std::cout, make_error_frame(
                            req_id, "candidate", "INVALID_POOL_OVERRIDES",
                            error.what()));
                        return;
                    }
                }
                candidates.push_back(std::move(cand));
            }
        }

        std::sort(
            candidates.begin(),
            candidates.end(),
            [](const auto& lhs, const auto& rhs) {
                return lhs.ordinal < rhs.ordinal;
            }
        );

        // Execute batch evaluation via core
        auto batch_result = curve_fx::evaluator::evaluate_batch_candidates(
            *session_->store,
            session_->config,
            candidates,
            obs_spec
        );

        auto t1 = std::chrono::high_resolution_clock::now();
        double elapsed_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        // Build response JSON frame
        json::object resp;
        resp["protocol"] = "curve_fx_eval";
        resp["type"] = "batch_result";
        resp["request_id"] = req_id;
        resp["session_id"] = session_->session_id;
        resp["status"] = "complete";
        resp["metric_projection"] = full_metric_projection ? "full" : "summary";
        if (metrics_format == "array") {
            json::array fields;
            for (const auto& name : metric_fields) fields.push_back(json::value(name));
            resp["metric_fields"] = std::move(fields);
        }

        json::array results_arr;
        for (size_t c_idx = 0; c_idx < batch_result.candidate_results.size(); ++c_idx) {
            const auto& res = batch_result.candidate_results[c_idx];
            const uint64_t response_ordinal = res.ordinal;
            json::object r;
            r["ordinal"] = response_ordinal;
            r["candidate_id"] = res.candidate_id;
            r["status"] = res.success ? "ok" : "failed";

            if (!res.success) {
                r["error"] = res.error_message;
            }

            // Raw metrics dictionary in canonical field order.
            if (metrics_format == "array") {
                json::array values;
                for (const auto& field_name : metric_fields) {
                    const auto it = res.aggregate_metrics.find(field_name);
                    values.push_back(
                        it != res.aggregate_metrics.end() ? it->second : -1.0
                    );
                }
                r["metrics"] = std::move(values);
            } else {
                json::object metrics_obj;
                for (const auto& field_name : metric_fields) {
                    const auto it = res.aggregate_metrics.find(field_name);
                    metrics_obj[field_name] =
                        it != res.aggregate_metrics.end() ? it->second : -1.0;
                }
                r["metrics"] = std::move(metrics_obj);
            }

            // Scenario results: populated only when metric_projection is "full"
            json::array sc_results;
            if (full_metric_projection) {
                for (const auto& sc_res : res.scenario_results) {
                    json::object s_obj;
                    s_obj["scenario_id"] = sc_res.scenario_id;
                    s_obj["status"] = sc_res.success ? "ok" : "failed";
                    if (!sc_res.success) {
                        s_obj["error"] = sc_res.error_message;
                    }
                    json::object sc_m;
                    for (const auto& kv : sc_res.metrics) {
                        sc_m[kv.first] = kv.second;
                    }
                    s_obj["metrics"] = sc_m;
                    sc_results.push_back(s_obj);
                }
            }
            r["scenario_results"] = sc_results;

            // A direct session has exactly one scenario, so its sidecars are
            // already an unambiguous candidate result.
            if (obs_spec.kind == curve_fx::evaluator::ObservationKind::FullTrace && !artifact_dir.empty() && res.success) {
                json::object art_obj;
                fs::path base_art_path(artifact_dir);
                bool all_writes_ok = true;
                std::vector<fs::path> written_paths;
                const auto cleanup_sidecars = [&]() {
                    for (const auto& path : written_paths) {
                        std::error_code ignored;
                        fs::remove(path, ignored);
                    }
                };

                if (res.scenario_results.size() != 1 || !res.scenario_results.front().has_trace) {
                    all_writes_ok = false;
                } else {
                    const auto& sc_res = res.scenario_results.front();
                    if (!is_safe_identifier(sc_res.scenario_id)) {
                        write_frame(std::cout, make_error_frame(req_id, "sidecar", "INVALID_SCENARIO_ID",
                            "scenario_id contains unsafe path characters: " + sc_res.scenario_id));
                        return;
                    }

                    const std::string stem = "candidate_" +
                        std::to_string(response_ordinal) + "." + sc_res.scenario_id;
                    const fs::path trace_path = base_art_path / (stem + ".trace.json");
                    const bool trace_preexisting = fs::exists(trace_path);
                    all_writes_ok = write_atomic_file(trace_path, sc_res.trace_json);
                    if (all_writes_ok) {
                        if (!trace_preexisting) written_paths.push_back(trace_path);
                        art_obj["trace_path"] = trace_path.string();
                        art_obj["effective_inputs"] = sc_res.effective_inputs;
                    }

                    if (all_writes_ok && !sc_res.actions_json.empty()) {
                        const fs::path actions_path = base_art_path / (stem + ".actions.json");
                        const bool actions_preexisting = fs::exists(actions_path);
                        all_writes_ok = write_atomic_file(actions_path, sc_res.actions_json);
                        if (all_writes_ok) {
                            if (!actions_preexisting) written_paths.push_back(actions_path);
                            art_obj["actions_path"] = actions_path.string();
                        }
                    }
                }

                if (!all_writes_ok) {
                    cleanup_sidecars();
                    r["artifacts"] = nullptr;
                } else {
                    r["artifacts"] = art_obj;
                }
            } else {
                r["artifacts"] = nullptr;
            }

            results_arr.push_back(r);
        }

        resp["results"] = results_arr;
        resp["elapsed_ms"] = elapsed_ms;

        write_frame(std::cout, resp);
    }

    void handle_close_session(const json::object& req, const std::string& req_id) {
        if (const auto field = unknown_field(req, {
                "protocol", "type", "request_id", "session_id"
            })) {
            write_frame(std::cout, make_error_frame(
                req_id, "protocol", "UNKNOWN_FIELD",
                "close_session contains unknown field: " + *field));
            return;
        }
        const std::string requested_session_id = arb::get_string_opt(
            req, "session_id", "");
        if (session_.has_value() && requested_session_id != session_->session_id) {
            write_frame(std::cout, make_error_frame(
                req_id, "session", "SESSION_MISMATCH",
                "close_session session_id does not match the active session"));
            return;
        }
        std::string sid = "none";
        if (session_.has_value()) {
            sid = session_->session_id;
            session_.reset();
        }
        json::object resp;
        resp["protocol"] = "curve_fx_eval";
        resp["type"] = "session_closed";
        resp["request_id"] = req_id;
        resp["session_id"] = sid;
        write_frame(std::cout, resp);
    }
};

} // namespace server
} // namespace curve_fx

int main(int argc, char* argv[]) {
    bool identity_only = false;
    bool describe_only = false;
    std::string mode = "serve";
    bool mode_explicit = false;
    size_t worker_count = 1;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--identity-json") {
            identity_only = true;
            continue;
        }
        if (arg == "--describe-json") {
            describe_only = true;
            continue;
        }
        if (arg == "--workers") {
            if (++i >= argc) {
                std::cerr << "Error: --workers requires a positive integer\n";
                return 1;
            }
            const std::string value = argv[i];
            if (value.empty() || !std::all_of(
                    value.begin(), value.end(), [](unsigned char c) {
                        return c >= '0' && c <= '9';
                    })) {
                std::cerr << "Error: --workers requires a positive integer\n";
                return 1;
            }
            try {
                const auto parsed = std::stoull(value);
                if (parsed == 0 || parsed > std::numeric_limits<size_t>::max()) {
                    throw std::out_of_range("worker count");
                }
                worker_count = static_cast<size_t>(parsed);
            } catch (const std::exception&) {
                std::cerr << "Error: --workers requires a positive integer\n";
                return 1;
            }
            continue;
        }
        if (arg == "-h" || arg == "--help") {
            std::cerr << "Usage: " << argv[0]
                      << " [serve | --identity-json | --describe-json] [--workers N]\n\n"
                      << "Modes:\n"
                      << "  serve              Run persistent NDJSON server implementing protocol curve_fx_eval (stdin/stdout)\n"
                      << "  --identity-json    Print evaluator identity frame to stdout and exit 0\n"
                      << "  --describe-json    Print executable-bound build and lowering schema and exit 0\n"
                      << "Options:\n"
                      << "  --workers N        Use N evaluator workers (default 1; cannot exceed detected hardware concurrency)\n";
            return 0;
        }
        if (arg == "serve" && !mode_explicit) {
            mode = arg;
            mode_explicit = true;
            continue;
        }
        std::cerr << "Error: Unknown argument '" << arg
                  << "'. Use --help for usage.\n";
        return 1;
    }

    if (identity_only && describe_only) {
        std::cerr << "Error: --identity-json and --describe-json are mutually exclusive\n";
        return 1;
    }

    try {
        curve_fx::evaluator::configure_worker_count(worker_count);
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << "\n";
        return 1;
    }

    if (identity_only) {
        write_frame(std::cout, make_hello_frame());
        return 0;
    }

    if (describe_only) {
        write_frame(std::cout, make_description());
        return 0;
    }

    if (mode == "serve") {
        try {
            curve_fx::server::EvaluatorServer server;
            server.run();
            return 0;
        } catch (const std::exception& error) {
            std::cerr << "Error: " << error.what() << "\n";
            return 1;
        }
    }
    return 1;
}
