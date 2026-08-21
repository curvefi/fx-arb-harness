// curve-fx-arb-harness - Unified evaluator executable (arb_evaluator_ld)
//
// Modes:
//   --identity-json  : Emit evaluator identity and protocol capabilities to stdout and exit 0.
//   --describe-json  : Emit executable-bound build and lowering schema to stdout and exit 0.
//   serve            : Persistent NDJSON server implementing protocol curve_fx_eval_v1 over stdin/stdout.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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
#include "core/sha256.hpp"
#include "curve_fx_evaluator/compiled_policy_identity.hpp"
#include "curve_fx_evaluator/evaluator.hpp"
#include "curve_fx_evaluator/parameter_schema.hpp"
#include "pools/pool_config_parse.hpp"

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
    "curve_fx_eval_v1 requires IEEE-754 binary64 wire inputs"
);

using SelectedPolicy = arb::pools::twocrypto_fx::ChallengeFeePolicy<RealT>;
static constexpr std::size_t SELECTED_POLICY_PARAM_COUNT =
    SelectedPolicy::DESCRIPTOR.size();
static constexpr std::string_view SELECTED_POLICY_ID =
    SelectedPolicy::DESCRIPTOR.name;
static_assert(
    SELECTED_POLICY_PARAM_COUNT <=
        arb::pools::twocrypto_fx::PolicyConfig<RealT>{}.params.size(),
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

namespace {

static constexpr size_t MAX_FRAME_BYTES = 4 * 1024 * 1024; // 4 MiB

static const std::vector<std::string> CANONICAL_METRIC_FIELDS = {
    "vp", "xcp_profit", "lp_xcp_profit", "apy", "apy_net", "apy_net_gm",
    "avg_rel_price_diff", "max_rel_price_diff", "max_7d_rel_price_diff", "final_rel_price_diff",
    "max_7d_skew", "min_price_scale", "max_price_scale", "tw_avg_pool_fee", "min_pool_fee",
    "max_pool_fee", "tw_real_slippage_1pct", "tw_real_slippage_5pct", "tw_real_slippage_10pct",
    "trades", "n_rebalances", "dynamic_keeper_attempts", "dynamic_keeper_commits",
    "dynamic_keeper_gap_checks", "dynamic_keeper_gap_fires", "dynamic_keeper_gap_threshold_fires",
    "dynamic_keeper_heartbeat_fires", "dynamic_keeper_commit_clock_fires",
    "dynamic_keeper_attempts_per_day", "dynamic_keeper_commits_per_day",
    "dynamic_keeper_gap_checks_per_day", "dynamic_keeper_gap_fires_per_day",
    "dynamic_keeper_gap_threshold_fires_per_day", "dynamic_keeper_heartbeat_fires_per_day",
    "dynamic_keeper_commit_clock_fires_per_day", "dynamic_keeper_step_bps_avg",
    "dynamic_keeper_step_bps_max", "policy_keeper_checks", "policy_keeper_reject_clock",
    "policy_keeper_reject_target_unavailable", "policy_keeper_reject_deadband",
    "policy_keeper_reject_step_min", "policy_keeper_reject_below_threshold",
    "policy_keeper_reject_block", "policy_keeper_reject_outer_profit",
    "policy_keeper_raw_gap_candidates", "policy_keeper_submissions",
    "policy_keeper_submitted_commits", "policy_keeper_final_lp_rejects",
    "policy_keeper_unexpected_step_rejects", "policy_keeper_exceptions",
    "policy_keeper_lp_below_precision", "policy_keeper_lp_below_floor",
    "policy_keeper_lp_burn_cap_exhausted", "policy_keeper_direction_up",
    "policy_keeper_direction_down", "policy_keeper_submissions_per_day",
    "policy_keeper_final_lp_rejects_per_day", "policy_keeper_fire_to_commit_ratio",
    "arb_edge_candidates", "arb_invalid_size_rejections", "arb_nonpositive_profit_rejections",
    "arb_guarded_loss_coin0", "yb_enabled", "yb_apy", "yb_apy_gm", "yb_final_growth", "yb_fee",
    "yb_releverage_trades", "yb_gm_windows", "yb_gm_floored_windows", "yb_gm_floor_share",
    "elapsed_ms", "duration_s", "total_notional_coin0", "lp_fee_coin0", "arb_pnl_coin0",
    "fee_capture_rate", "donations", "donation_coin0_total", "avg_imbalance",
    "max_episode_gap_energy", "detach_energy", "detach_energy_ungated",
    "detach_energy_ungated_3pct", "detach_energy_ungated_5pct", "detach_energy_short3h",
    "tvl_growth",
    "keeper_successful_submissions", "fixed_keeper_ticks"
};

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
    const bool textual = field == "tag" || field == "kind" ||
        field == "price_source" || field == "policy";
    if (!textual && arb::pools::is_number_or_string(value)) {
        return json::value(arb::canonical_float_string(
            arb::parse_input_double(value)));
    }
    return value;
}

json::object normalize_pool_override_identity(const json::object& value) {
    return normalize_pool_override_identity_value(value, "").as_object();
}

std::string compute_binary_sha256(const char* argv0) {
    if (argv0 == nullptr || *argv0 == '\0') {
        throw std::runtime_error("cannot attest evaluator binary: argv[0] is empty");
    }
    const fs::path requested(argv0);
    std::vector<fs::path> candidates;
    candidates.push_back(requested);
    if (!requested.has_parent_path()) {
        if (const char* path_env = std::getenv("PATH")) {
            std::stringstream paths(path_env);
            std::string directory;
            while (std::getline(paths, directory, ':')) {
                if (!directory.empty()) candidates.emplace_back(fs::path(directory) / requested);
            }
        }
    }
    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec) && !ec) {
            return arb::core::sha256_file(fs::canonical(candidate).string());
        }
    }
    throw std::runtime_error(
        "cannot attest evaluator binary: executable path was not found: " +
        requested.string());
}

std::string compute_deterministic_candidate_fingerprint(
    const std::string& session_fingerprint,
    const std::vector<RealT>& policy_params,
    const json::object& pool_overrides,
    const std::map<std::string, double>& aggregate_metrics
) {
    json::object fp_obj;
    fp_obj["session"] = session_fingerprint;

    json::array params_arr;
    for (auto p : policy_params) {
        params_arr.push_back(json::value(arb::canonical_binary64_string(p)));
    }
    fp_obj["params"] = params_arr;
    fp_obj["overrides"] = normalize_pool_override_identity(pool_overrides);

    // Include all deterministic economic metrics, strictly excluding elapsed_ms and non-deterministic timing
    json::object econ_metrics;
    for (const auto& [k, v] : aggregate_metrics) {
        if (k != "elapsed_ms") {
            econ_metrics[k] = v;
        }
    }
    fp_obj["metrics"] = econ_metrics;
    return arb::sha256_canonical_json(fp_obj);
}

std::string compute_session_config_sha256(
    const curve_fx::evaluator::SessionConfig<RealT>& cfg,
    size_t pool_index
) {
    json::object value;
    value["pool_index"] = pool_index;
    value["min_swap_frac"] = arb::canonical_binary64_string(cfg.min_swap_frac);
    value["max_swap_frac"] = arb::canonical_binary64_string(cfg.max_swap_frac);
    value["dustswap_freq_s"] = cfg.dustswap_freq_s;
    value["dustswap_random"] = cfg.dustswap_random;
    value["dustswap_dynamic_freq_s"] = cfg.dustswap_dynamic_freq_s;
    value["dustswap_dynamic_gap_enabled"] = cfg.dustswap_dynamic_gap_enabled;
    value["dustswap_dynamic_gap_bps"] =
        arb::canonical_binary64_string(cfg.dustswap_dynamic_gap_bps);
    value["dustswap_dynamic_heartbeat_s"] = cfg.dustswap_dynamic_heartbeat_s;
    value["dustswap_commit_clock_freq_s"] = cfg.dustswap_commit_clock_freq_s;
    value["policy_keeper_enabled"] = cfg.policy_keeper_enabled;
    value["allow_hybrid_keeper"] = cfg.allow_hybrid_keeper;
    value["user_swap_freq_s"] = cfg.user_swap_freq_s;
    value["user_swap_size_frac"] =
        arb::canonical_binary64_string(cfg.user_swap_size_frac);
    value["user_swap_thresh"] =
        arb::canonical_binary64_string(cfg.user_swap_thresh);
    value["enable_slippage_probes"] = cfg.enable_slippage_probes;
    value["yb_mode"] = cfg.yb_mode;
    value["yb_releverage_fee"] =
        arb::canonical_binary64_string(cfg.yb_releverage_fee);
    value["yb_cash_multiplier"] =
        arb::canonical_binary64_string(cfg.yb_cash_multiplier);
    return arb::sha256_canonical_json(value);
}

json::object make_evaluator_identity(const std::string& bin_hash) {
    json::object id;
    id["binary_sha256"] = bin_hash;
    id["harness_version"] = curve_fx::identity::HARNESS_VERSION;
    id["pool_version"] = curve_fx::identity::POOL_VERSION;
    id["policy_id"] = std::string(SELECTED_POLICY_ID);
    id["policy_source_sha256"] = curve_fx::identity::POLICY_SOURCE_SHA256;
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

json::object make_description(const std::string& bin_hash) {
    json::object info;
    info["schema_version"] = "curve_fx_evaluator_description_v1";
    info["binary_sha256"] = bin_hash;

    json::object harness;
    harness["version"] = curve_fx::identity::HARNESS_VERSION;
    harness["revision"] = curve_fx::identity::HARNESS_GIT_REVISION;
    harness["dirty"] = source_dirty_value(
        curve_fx::identity::HARNESS_GIT_DIRTY);
    info["harness"] = std::move(harness);

    json::object pool;
    pool["version"] = curve_fx::identity::POOL_VERSION;
    pool["revision"] = curve_fx::identity::POOL_GIT_REVISION;
    pool["dirty"] = source_dirty_value(curve_fx::identity::POOL_GIT_DIRTY);
    info["pool"] = std::move(pool);

    json::object policy;
    policy["id"] = std::string(SELECTED_POLICY_ID);
    policy["abi"] = curve_fx::identity::POLICY_ABI;
    policy["source_sha256"] = curve_fx::identity::POLICY_SOURCE_SHA256;
    policy["parameter_count"] = SELECTED_POLICY_PARAM_COUNT;
    policy["descriptor_abi_version"] =
        arb::pools::twocrypto_fx::POLICY_DESCRIPTOR_ABI_VERSION;
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
    info["parameter_schema_sha256"] = arb::crypto::sha256_hex(
        schema_canonical_json.data(), schema_canonical_json.size());
    info["parameter_schema_canonical_json"] = schema_canonical_json;
    info["parameter_schema"] = std::move(schema);
    return info;
}

json::object make_hello_frame(const std::string& bin_hash) {
    json::object hello;
    hello["protocol"] = "curve_fx_eval_v1";
    hello["type"] = "hello";
    hello["version"] = 1;
    hello["evaluator_identity"] = make_evaluator_identity(bin_hash);

    json::array caps;
    caps.push_back("summary");
    caps.push_back("full_trace");
    caps.push_back("atomic_sidecars");
    hello["capabilities"] = caps;

    json::array yb_modes;
    yb_modes.push_back("off");
    yb_modes.push_back("passive");
    yb_modes.push_back("active_2l");
    hello["yb_modes"] = yb_modes;

    hello["metric_schema"] = "twocrypto-summary-v1";

    json::array fields;
    for (const auto& f : CANONICAL_METRIC_FIELDS) {
        fields.push_back(json::value(f));
    }
    hello["metric_fields"] = fields;

    json::object limits;
    limits["max_frame_bytes"] = MAX_FRAME_BYTES;
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
    err["protocol"] = "curve_fx_eval_v1";
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

bool is_path_traversal(const std::string& path) {
    if (path.empty()) return false;
    if (path.front() == '/' || path.front() == '\\') return true;
    fs::path p(path);
    for (const auto& part : p) {
        if (part == "..") return true;
    }
    return false;
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

bool resolve_confined_file_path(const fs::path& base_dir, const std::string& relative_filename, fs::path& out_path) {
    if (relative_filename.empty() || is_path_traversal(relative_filename)) {
        return false;
    }
    fs::path target = base_dir / relative_filename;
    fs::path norm_root = fs::weakly_canonical(fs::current_path());
    fs::path norm_base = fs::weakly_canonical(base_dir);
    fs::path norm_target = fs::weakly_canonical(target);

    auto [r_it, base_it] = std::mismatch(
        norm_root.begin(), norm_root.end(), norm_base.begin(), norm_base.end());
    if (r_it != norm_root.end()) {
        return false;
    }
    auto [b_it, t_it] = std::mismatch(norm_base.begin(), norm_base.end(), norm_target.begin(), norm_target.end());
    if (b_it != norm_base.end()) {
        return false;
    }
    out_path = norm_target;
    return true;
}

bool is_valid_hex64(std::string_view s) {
    if (s.size() != 64) return false;
    for (char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

bool parse_size_t_field(
    const json::object& object,
    const char* key,
    size_t fallback,
    size_t& result
) {
    const auto* value = object.if_contains(key);
    if (value == nullptr) {
        result = fallback;
        return true;
    }
    if (value->is_uint64()) {
        const uint64_t parsed = value->as_uint64();
        if (parsed > std::numeric_limits<size_t>::max()) return false;
        result = static_cast<size_t>(parsed);
        return true;
    }
    if (value->is_int64() && value->as_int64() >= 0) {
        result = static_cast<size_t>(value->as_int64());
        return true;
    }
    return false;
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

bool write_atomic_file(const fs::path& p, const std::string& content, std::string& out_sha256) {
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

    out_sha256 = arb::core::sha256_file(tmp_p.string());
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
    std::string template_path;
    std::string manifest_path;
    std::string scenario_set_sha256;
    std::string session_config_sha256;
    std::string session_fingerprint;
    std::shared_ptr<curve_fx::evaluator::ScenarioStore<RealT>> store;
    curve_fx::evaluator::SessionConfig<RealT> config;
};

class EvaluatorServer {
public:
    explicit EvaluatorServer(std::string binary_hash)
        : bin_hash_(std::move(binary_hash)) {}

    void run() {
        std::cerr << "[evaluator] Worker count: "
                  << curve_fx::evaluator::configured_worker_count() << "\n";
        // Emit initial hello frame
        write_frame(std::cout, make_hello_frame(bin_hash_));

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

            if (protocol != "curve_fx_eval_v1") {
                write_frame(
                    std::cout,
                    make_error_frame(req_id, "protocol", "PROTOCOL_MISMATCH",
                        "protocol must be 'curve_fx_eval_v1'")
                );
                continue;
            }

            if (type == "open_session") {
                handle_open_session(req, req_id);
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
    std::string bin_hash_;
    std::optional<ActiveSession> session_;
    bool session_ever_opened_{false};

    void handle_open_session(const json::object& req, const std::string& req_id) {
        if (const auto field = unknown_field(req, {
                "protocol", "type", "request_id", "session_id",
                "template_path", "template_sha256", "manifest_path",
                "manifest_sha256", "pool_index", "n_candles", "start_time",
                "end_time", "candle_filter", "min_swap", "max_swap",
                "dustswap_freq_s", "dustswap_random",
                "dustswap_dynamic_freq_s", "dustswap_dynamic_gap_enabled",
                "dustswap_dynamic_gap_bps", "dustswap_dynamic_heartbeat_s",
                "dustswap_commit_clock_freq_s", "policy_keeper_enabled",
                "allow_hybrid_keeper", "user_swap_freq_s",
                "user_swap_size_frac", "user_swap_thresh",
                "disable_slippage_probes", "yb_releverage",
                "yb_releverage_fee", "yb_cash_multiplier", "yb_mode"
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
        const std::string tpl_hash = arb::get_string_opt(req, "template_sha256", "");
        const std::string man_path = arb::get_string_opt(req, "manifest_path", "");
        const std::string man_hash = arb::get_string_opt(req, "manifest_sha256", "");

        if (tpl_path.empty() || man_path.empty()) {
            write_frame(std::cout, make_error_frame(req_id, "session", "INVALID_ARGUMENT", "template_path and manifest_path are required"));
            return;
        }

        if (!is_valid_hex64(tpl_hash)) {
            write_frame(std::cout, make_error_frame(req_id, "session", "INVALID_ARGUMENT",
                "template_sha256 must be a 64-character hex SHA-256 digest"));
            return;
        }

        if (!is_valid_hex64(man_hash)) {
            write_frame(std::cout, make_error_frame(req_id, "session", "INVALID_ARGUMENT",
                "manifest_sha256 must be a 64-character hex SHA-256 digest"));
            return;
        }

        // Verify template attestation
        if (!fs::exists(tpl_path) || fs::is_directory(tpl_path)) {
            write_frame(std::cout, make_error_frame(req_id, "session", "FILE_NOT_FOUND", "Template file not found: " + tpl_path));
            return;
        }
        const std::string actual_tpl_hash = arb::core::sha256_file(tpl_path);
        if (actual_tpl_hash != tpl_hash) {
            write_frame(std::cout, make_error_frame(req_id, "session", "ATTESTATION_FAILED",
                "Template SHA-256 mismatch: expected " + tpl_hash + ", got " + actual_tpl_hash));
            return;
        }

        // Verify manifest attestation
        if (!fs::exists(man_path) || fs::is_directory(man_path)) {
            write_frame(std::cout, make_error_frame(req_id, "session", "FILE_NOT_FOUND", "Manifest file not found: " + man_path));
            return;
        }
        const std::string actual_man_hash = arb::core::sha256_file(man_path);
        if (actual_man_hash != man_hash) {
            write_frame(std::cout, make_error_frame(req_id, "session", "ATTESTATION_FAILED",
                "Manifest SHA-256 mismatch: expected " + man_hash + ", got " + actual_man_hash));
            return;
        }

        size_t pool_index = 0;
        if (!parse_size_t_field(req, "pool_index", 0, pool_index)) {
            write_frame(std::cout, make_error_frame(
                req_id, "session", "INVALID_ARGUMENT",
                "pool_index must be a non-negative integer"));
            return;
        }

        try {
            auto store = std::make_shared<curve_fx::evaluator::ScenarioStore<RealT>>();
            curve_fx::evaluator::ScenarioLoadOptions opts{};
            opts.pool_index = pool_index;
            opts.max_candles = static_cast<size_t>(arb::get_double_opt(req, "n_candles", 0.0));
            opts.start_ts = static_cast<uint64_t>(arb::get_double_opt(req, "start_time", 0.0));
            opts.end_ts = static_cast<uint64_t>(arb::get_double_opt(req, "end_time", 0.0));
            opts.candle_filter_pct = arb::get_double_opt(req, "candle_filter", 0.0);

            std::cerr << "[evaluator] Loading scenarios from manifest: " << man_path << " with template: " << tpl_path << "\n";
            store->load_from_manifest(man_path, tpl_path, opts);

            curve_fx::evaluator::SessionConfig<RealT> cfg{};
            cfg.min_swap_frac = static_cast<RealT>(arb::get_double_opt(req, "min_swap", 1e-6));
            cfg.max_swap_frac = static_cast<RealT>(arb::get_double_opt(req, "max_swap", 1.0));
            cfg.dustswap_freq_s = static_cast<uint64_t>(arb::get_double_opt(req, "dustswap_freq_s", 3600));
            cfg.dustswap_random = req.if_contains("dustswap_random") && req.at("dustswap_random").as_bool();
            cfg.dustswap_dynamic_freq_s = static_cast<uint64_t>(arb::get_double_opt(req, "dustswap_dynamic_freq_s", 0));
            cfg.dustswap_dynamic_gap_enabled =
                req.if_contains("dustswap_dynamic_gap_enabled") &&
                req.at("dustswap_dynamic_gap_enabled").as_bool();
            cfg.dustswap_dynamic_gap_bps = static_cast<RealT>(
                arb::get_double_opt(req, "dustswap_dynamic_gap_bps", 0.0));
            cfg.dustswap_dynamic_heartbeat_s = static_cast<uint64_t>(
                arb::get_double_opt(req, "dustswap_dynamic_heartbeat_s", 0.0));
            cfg.dustswap_commit_clock_freq_s = static_cast<uint64_t>(
                arb::get_double_opt(req, "dustswap_commit_clock_freq_s", 0.0));
            cfg.policy_keeper_enabled =
                req.if_contains("policy_keeper_enabled") &&
                req.at("policy_keeper_enabled").as_bool();
            cfg.allow_hybrid_keeper =
                req.if_contains("allow_hybrid_keeper") &&
                req.at("allow_hybrid_keeper").as_bool();
            cfg.user_swap_freq_s = static_cast<uint64_t>(arb::get_double_opt(req, "user_swap_freq_s", 0));
            cfg.user_swap_size_frac = static_cast<RealT>(arb::get_double_opt(req, "user_swap_size_frac", 0.01));
            cfg.user_swap_thresh = static_cast<RealT>(arb::get_double_opt(req, "user_swap_thresh", 0.05));
            cfg.enable_slippage_probes = !(req.if_contains("disable_slippage_probes") && req.at("disable_slippage_probes").as_bool());
            std::string yb_mode = "off";
            if (req.if_contains("yb_mode")) {
                if (!req.at("yb_mode").is_string()) {
                    write_frame(std::cout, make_error_frame(
                        req_id, "protocol", "INVALID_ARGUMENT",
                        "yb_mode must be a string"));
                    return;
                }
                yb_mode = std::string(req.at("yb_mode").as_string());
                if (yb_mode != "off" && yb_mode != "passive" &&
                    yb_mode != "active_2l") {
                    write_frame(std::cout, make_error_frame(
                        req_id, "protocol", "INVALID_ARGUMENT",
                        "yb_mode must be one of 'off', 'passive', 'active_2l'"));
                    return;
                }
            } else if (req.if_contains("yb_releverage")) {
                if (!req.at("yb_releverage").is_bool()) {
                    write_frame(std::cout, make_error_frame(
                        req_id, "protocol", "INVALID_ARGUMENT",
                        "yb_releverage must be a boolean"));
                    return;
                }
                if (req.at("yb_releverage").as_bool()) {
                    yb_mode = "active_2l";
                }
            }
            cfg.yb_mode = yb_mode;
            cfg.yb_releverage_fee = static_cast<RealT>(arb::get_double_opt(req, "yb_releverage_fee", 0.012));
            cfg.yb_cash_multiplier = static_cast<RealT>(
                arb::get_double_opt(req, "yb_cash_multiplier", 1.0));

            ActiveSession sess;
            sess.session_id = session_id;
            sess.template_path = tpl_path;
            sess.manifest_path = man_path;
            sess.store = store;
            sess.config = cfg;
            sess.scenario_set_sha256 = store->compute_scenario_set_sha256();
            sess.session_config_sha256 = compute_session_config_sha256(
                cfg, opts.pool_index);
            sess.session_fingerprint = store->compute_session_fingerprint(
                bin_hash_,
                curve_fx::identity::POLICY_SOURCE_SHA256,
                sess.session_config_sha256,
                opts.pool_index
            );

            session_ = std::move(sess);
            session_ever_opened_ = true;

            json::object resp;
            resp["protocol"] = "curve_fx_eval_v1";
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
            resp["scenario_set_sha256"] = session_->scenario_set_sha256;
            resp["session_fingerprint"] = session_->session_fingerprint;
            resp["session_config_sha256"] = session_->session_config_sha256;
            resp["metric_schema_sha256"] = arb::core::sha256_bytes("twocrypto-summary-v1");

            write_frame(std::cout, resp);
            std::cerr << "[evaluator] Session '" << session_id << "' initialized successfully with "
                      << store->scenarios().size() << " scenarios.\n";

        } catch (const std::exception& e) {
            write_frame(std::cout, make_error_frame(req_id, "session", "SESSION_INIT_FAILED", e.what()));
        }
    }

    void handle_evaluate_batch(const json::object& req, const std::string& req_id) {
        if (const auto field = unknown_field(req, {
                "protocol", "type", "request_id", "session_id",
                "metric_projection", "observation", "candidates"
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
        if (!req.if_contains("candidates") || !req.at("candidates").is_array()) {
            write_frame(std::cout, make_error_frame(req_id, "candidate", "INVALID_ARGUMENT", "candidates array is required"));
            return;
        }

        const auto& cand_arr = req.at("candidates").as_array();
        if (cand_arr.empty()) {
            write_frame(std::cout, make_error_frame(req_id, "candidate", "EMPTY_BATCH", "candidates array cannot be empty"));
            return;
        }

        // No candidate-count cap: the 4 MiB request-frame guard bounds the
        // batch. Cluster grids batch up to chunk-size candidates (e.g. 1024)
        // to saturate blade worker pools.

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
            const double trace_interval = arb::get_double_opt(
                obs_obj, "trace_interval", 1.0);
            if (!std::isfinite(trace_interval) || trace_interval < 1.0 ||
                std::floor(trace_interval) != trace_interval) {
                write_frame(std::cout, make_error_frame(
                    req_id, "protocol", "INVALID_OBSERVATION",
                    "observation.trace_interval must be a positive integer"));
                return;
            }
            obs_spec.trace_interval = static_cast<size_t>(trace_interval);
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

            if (is_path_traversal(artifact_dir)) {
                write_frame(std::cout, make_error_frame(req_id, "sidecar", "PATH_TRAVERSAL_DETECTED",
                    "artifact_dir must be a relative path and cannot contain '..' or root: " + artifact_dir));
                return;
            }
        }

        auto t0 = std::chrono::high_resolution_clock::now();

        // Convert candidate specs
        std::vector<curve_fx::evaluator::EvaluationCandidate<RealT>> candidates;
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
            const auto* ordinal_value = c_obj.if_contains("ordinal");
            double ordinal = -1.0;
            if (ordinal_value != nullptr) {
                if (ordinal_value->is_int64()) {
                    ordinal = static_cast<double>(ordinal_value->as_int64());
                } else if (ordinal_value->is_uint64()) {
                    ordinal = static_cast<double>(ordinal_value->as_uint64());
                }
            }
            if (ordinal < 0.0 || ordinal > std::numeric_limits<uint32_t>::max() ||
                std::floor(ordinal) != ordinal) {
                write_frame(std::cout, make_error_frame(
                    req_id, "candidate", "INVALID_ORDINAL",
                    "candidate ordinal must be an unsigned 32-bit integer"));
                return;
            }
            cand.ordinal = static_cast<uint32_t>(ordinal);
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
        resp["protocol"] = "curve_fx_eval_v1";
        resp["type"] = "batch_result";
        resp["request_id"] = req_id;
        resp["session_id"] = session_->session_id;
        resp["status"] = "complete";
        resp["metric_projection"] = full_metric_projection ? "full" : "summary";

        json::array results_arr;
        for (size_t c_idx = 0; c_idx < batch_result.candidate_results.size(); ++c_idx) {
            const auto& res = batch_result.candidate_results[c_idx];
            const auto& cand_req = candidates[c_idx];

            json::object r;
            r["ordinal"] = res.ordinal;
            r["candidate_id"] = res.candidate_id;
            r["status"] = res.success ? "ok" : "failed";

            // Compute strictly deterministic economic fingerprint (excluding elapsed_ms)
            std::string econ_fp = compute_deterministic_candidate_fingerprint(
                session_->session_fingerprint,
                cand_req.policy_params,
                cand_req.pool_overrides,
                res.aggregate_metrics
            );
            r["economic_fingerprint"] = econ_fp;

            if (!res.success) {
                r["error"] = res.error_message;
            }

            // Raw metrics dictionary and canonical vector
            json::object metrics_obj;
            json::array metrics_vec;
            for (const auto& field_name : CANONICAL_METRIC_FIELDS) {
                auto it = res.aggregate_metrics.find(field_name);
                if (it != res.aggregate_metrics.end()) {
                    metrics_obj[field_name] = it->second;
                    metrics_vec.push_back(it->second);
                } else {
                    metrics_obj[field_name] = -1.0;
                    metrics_vec.push_back(-1.0);
                }
            }
            r["metrics"] = metrics_obj;
            r["metrics_vec"] = metrics_vec;

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

            // Write full trace sidecars and unified manifest if requested
            if (obs_spec.kind == curve_fx::evaluator::ObservationKind::FullTrace && !artifact_dir.empty() && res.success) {
                json::object art_obj;
                fs::path base_art_path(artifact_dir);
                bool all_writes_ok = true;
                std::vector<fs::path> written_paths;
                const auto cleanup_bundle = [&]() {
                    for (const auto& path : written_paths) {
                        std::error_code ignored;
                        fs::remove(path, ignored);
                    }
                };

                json::object scenarios_manifest_map;
                std::string first_trace_path;
                std::string first_trace_sha256;
                std::string first_actions_path;
                std::string first_actions_sha256;

                for (const auto& sc_res : res.scenario_results) {
                    if (sc_res.has_trace) {
                        if (!is_safe_identifier(sc_res.scenario_id)) {
                            write_frame(std::cout, make_error_frame(req_id, "sidecar", "INVALID_SCENARIO_ID",
                                "scenario_id contains unsafe path characters: " + sc_res.scenario_id));
                            return;
                        }

                        const std::string expected_trace_sha256 =
                            arb::crypto::sha256_hex(sc_res.trace_json);
                        std::string rel_trace_name = econ_fp + "." +
                            sc_res.scenario_id + "." + expected_trace_sha256 +
                            ".trace.json";
                        fs::path trace_full_path;
                        if (!resolve_confined_file_path(base_art_path, rel_trace_name, trace_full_path)) {
                            write_frame(std::cout, make_error_frame(req_id, "sidecar", "PATH_TRAVERSAL_DETECTED",
                                "Trace path escaped artifact_dir: " + rel_trace_name));
                            return;
                        }

                        std::string trace_sha256;
                        const bool trace_preexisting = fs::exists(trace_full_path);
                        bool trace_written = write_atomic_file(trace_full_path, sc_res.trace_json, trace_sha256);
                        if (!trace_written || trace_sha256 != expected_trace_sha256) {
                            all_writes_ok = false;
                            break;
                        }
                        if (!trace_preexisting) written_paths.push_back(trace_full_path);
                        std::string rel_trace_path_str = artifact_dir + "/" + rel_trace_name;
                        if (first_trace_path.empty()) {
                            first_trace_path = rel_trace_path_str;
                            first_trace_sha256 = trace_sha256;
                        }

                        fs::path act_full_path;
                        std::string act_sha256;
                        bool act_written = false;
                        std::string rel_act_path_str;
                        if (!sc_res.actions_json.empty()) {
                            const std::string expected_actions_sha256 =
                                arb::crypto::sha256_hex(sc_res.actions_json);
                            std::string rel_act_name = econ_fp + "." +
                                sc_res.scenario_id + "." +
                                expected_actions_sha256 + ".actions.json";
                            if (resolve_confined_file_path(base_art_path, rel_act_name, act_full_path)) {
                                const bool actions_preexisting = fs::exists(act_full_path);
                                act_written = write_atomic_file(act_full_path, sc_res.actions_json, act_sha256);
                                if (!act_written || act_sha256 != expected_actions_sha256) {
                                    all_writes_ok = false;
                                    break;
                                }
                                if (!actions_preexisting) written_paths.push_back(act_full_path);
                                rel_act_path_str = artifact_dir + "/" + rel_act_name;
                                if (first_actions_path.empty()) {
                                    first_actions_path = rel_act_path_str;
                                    first_actions_sha256 = act_sha256;
                                }
                            } else {
                                all_writes_ok = false;
                                break;
                            }
                        }

                        json::object sc_desc;
                        json::object t_desc;
                        t_desc["path"] = rel_trace_path_str;
                        t_desc["sha256"] = trace_sha256;
                        t_desc["size_bytes"] = static_cast<int64_t>(sc_res.trace_json.size());
                        t_desc["record_count"] = static_cast<int64_t>(sc_res.trace_record_count);
                        sc_desc["trace"] = t_desc;

                        if (act_written) {
                            json::object a_desc;
                            a_desc["path"] = rel_act_path_str;
                            a_desc["sha256"] = act_sha256;
                            a_desc["size_bytes"] = static_cast<int64_t>(sc_res.actions_json.size());
                            a_desc["action_count"] = static_cast<int64_t>(sc_res.action_count);
                            sc_desc["actions"] = a_desc;
                        }

                        scenarios_manifest_map[sc_res.scenario_id] = sc_desc;
                    }
                }

                if (all_writes_ok && !scenarios_manifest_map.empty()) {
                    json::object trace_man;
                    trace_man["manifest_version"] = "curve_fx_trace_manifest_v1";
                    trace_man["session_id"] = session_->session_id;
                    trace_man["candidate_id"] = res.candidate_id;
                    trace_man["economic_fingerprint"] = econ_fp;
                    trace_man["scenarios"] = scenarios_manifest_map;

                    std::string man_json = json::serialize(trace_man);
                    const std::string expected_manifest_sha256 =
                        arb::crypto::sha256_hex(man_json);
                    const std::string rel_man_name = econ_fp + "." +
                        expected_manifest_sha256 + ".manifest.json";
                    fs::path man_full_path;
                    std::string man_sha256;
                    const bool manifest_path_resolved = resolve_confined_file_path(
                        base_art_path, rel_man_name, man_full_path);
                    const bool manifest_preexisting =
                        manifest_path_resolved && fs::exists(man_full_path);
                    if (!manifest_path_resolved ||
                        !write_atomic_file(man_full_path, man_json, man_sha256) ||
                        man_sha256 != expected_manifest_sha256) {
                        all_writes_ok = false;
                    } else {
                        if (!manifest_preexisting) written_paths.push_back(man_full_path);
                        art_obj["manifest_path"] = artifact_dir + "/" + rel_man_name;
                        art_obj["manifest_sha256"] = man_sha256;
                        art_obj["trace_path"] = first_trace_path;
                        art_obj["trace_sha256"] = first_trace_sha256;
                        if (!first_actions_path.empty()) {
                            art_obj["actions_path"] = first_actions_path;
                            art_obj["actions_sha256"] = first_actions_sha256;
                        }
                    }
                }

                if (!all_writes_ok || scenarios_manifest_map.empty()) {
                    cleanup_bundle();
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
        resp["protocol"] = "curve_fx_eval_v1";
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
                      << "  serve              Run persistent NDJSON server implementing protocol curve_fx_eval_v1 (stdin/stdout)\n"
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
        try {
            const std::string bin_hash = compute_binary_sha256(argv[0]);
            write_frame(std::cout, make_hello_frame(bin_hash));
            return 0;
        } catch (const std::exception& error) {
            std::cerr << "Error: " << error.what() << "\n";
            return 1;
        }
    }

    if (describe_only) {
        try {
            const std::string bin_hash = compute_binary_sha256(argv[0]);
            write_frame(std::cout, make_description(bin_hash));
            return 0;
        } catch (const std::exception& error) {
            std::cerr << "Error: " << error.what() << "\n";
            return 1;
        }
    }

    if (mode == "serve") {
        try {
            const std::string bin_hash = compute_binary_sha256(argv[0]);
            curve_fx::server::EvaluatorServer server(bin_hash);
            server.run();
            return 0;
        } catch (const std::exception& error) {
            std::cerr << "Error: " << error.what() << "\n";
            return 1;
        }
    }
    return 1;
}
