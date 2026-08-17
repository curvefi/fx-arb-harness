#include "curve_fx_evaluator/evaluator.hpp"

#include <boost/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace json = boost::json;
namespace fs = std::filesystem;

namespace curve_fx::evaluator {

namespace {


std::string resolve_manifest_path(const fs::path& manifest_dir, const std::string& target_path) {
    const fs::path target(target_path);
    if (target.is_absolute()) return target.string();
    return (manifest_dir / target).lexically_normal().string();
}

bool is_sha256(const std::string& digest) {
    if (digest.size() != 64) return false;
    return std::all_of(digest.begin(), digest.end(), [](unsigned char c) {
        return std::isxdigit(c) != 0;
    });
}

bool is_safe_scenario_id(const std::string& value) {
    if (value.empty()) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isalnum(c) != 0 || c == '-' || c == '_';
    });
}

} // namespace

template <typename T>
void ScenarioStore<T>::load_from_manifest(
    const std::string& manifest_path,
    const std::string& template_path,
    const ScenarioLoadOptions& opts
) {
    template_sha256_ = arb::core::sha256_file(template_path);

    std::ifstream man_file(manifest_path);
    if (!man_file) {
        throw std::runtime_error("Cannot open manifest file: " + manifest_path);
    }
    std::string man_content((std::istreambuf_iterator<char>(man_file)), std::istreambuf_iterator<char>());
    json::value man_val = json::parse(man_content);

    fs::path manifest_dir = fs::path(manifest_path).parent_path();

    // Load base pool template
    auto pool_doc = arb::pools::PoolConfigDocument::from_file(template_path);
    if (opts.pool_index >= pool_doc.size()) {
        throw std::runtime_error("Template pool_index " + std::to_string(opts.pool_index) +
                                 " out of range (total: " + std::to_string(pool_doc.size()) + ")");
    }
    auto [base_pool, base_costs] = pool_doc.template instantiate<T>(opts.pool_index);

    if (!man_val.is_object()) {
        throw std::runtime_error("Manifest must be an object");
    }
    const auto& manifest = man_val.as_object();
    arb::pools::reject_unknown_fields(
        manifest,
        {"schema_version", "run_kind", "run_id", "resolved_spec"},
        "manifest"
    );
    if (arb::get_string_opt(manifest, "schema_version", "") !=
        "fxsim_manifest_v1") {
        throw std::runtime_error("Manifest schema_version must be fxsim_manifest_v1");
    }
    if (arb::get_string_opt(manifest, "run_kind", "") != "session") {
        throw std::runtime_error("Manifest run_kind must be session");
    }
    if (arb::get_string_opt(manifest, "run_id", "").empty()) {
        throw std::runtime_error("Manifest run_id must be non-empty");
    }
    const auto* resolved_v = manifest.if_contains("resolved_spec");
    if (resolved_v == nullptr || !resolved_v->is_object()) {
        throw std::runtime_error("Manifest 'resolved_spec' field must be an object");
    }
    arb::pools::reject_unknown_fields(
        resolved_v->as_object(), {"scenario"}, "resolved_spec"
    );
    const auto* scenario_v = resolved_v->as_object().if_contains("scenario");
    if (scenario_v == nullptr || !scenario_v->is_object()) {
        throw std::runtime_error("Manifest 'resolved_spec.scenario' field must be an object");
    }
    json::array scen_entries;
    scen_entries.push_back(*scenario_v);

    scenarios_.clear();
    scenarios_.reserve(scen_entries.size());
    std::unordered_set<std::string> scenario_ids;

    for (size_t i = 0; i < scen_entries.size(); ++i) {
        if (!scen_entries[i].is_object()) {
            throw std::runtime_error("Scenario entry must be a JSON object");
        }
        const auto& sc_obj = scen_entries[i].as_object();
        arb::pools::reject_unknown_fields(
            sc_obj,
            {"id", "market_files", "start_time", "end_time", "n_candles", "candle_filter",
             "yb_mode", "yb_releverage"},
            "scenario"
        );
        std::string sc_id = arb::get_string_opt(sc_obj, "id", "scenario_" + std::to_string(i));
        if (!is_safe_scenario_id(sc_id)) {
            throw std::runtime_error("Scenario id contains unsafe characters: " + sc_id);
        }
        if (!scenario_ids.insert(sc_id).second) {
            throw std::runtime_error("Duplicate scenario id: " + sc_id);
        }

        // YieldBasis mode declared at scenario level (optional): canonical
        // string yb_mode, or the legacy boolean yb_releverage (true ->
        // "active_2l"). Accepted for manifest attestation; the authoritative
        // runtime mode still comes from open_session.
        std::string scen_yb_mode = "off";
        if (const auto* yb_mode_v = sc_obj.if_contains("yb_mode")) {
            if (!yb_mode_v->is_string()) {
                throw std::runtime_error(
                    "Scenario '" + sc_id + "' yb_mode must be a string"
                );
            }
            scen_yb_mode = std::string(yb_mode_v->as_string());
            if (scen_yb_mode != "off" && scen_yb_mode != "passive" &&
                scen_yb_mode != "active_2l") {
                throw std::runtime_error(
                    "Scenario '" + sc_id +
                    "' yb_mode must be one of off, passive, active_2l"
                );
            }
        }
        if (const auto* legacy_v = sc_obj.if_contains("yb_releverage")) {
            if (!legacy_v->is_bool()) {
                throw std::runtime_error(
                    "Scenario '" + sc_id + "' yb_releverage must be a boolean"
                );
            }
            if (legacy_v->as_bool() && scen_yb_mode == "off") {
                scen_yb_mode = "active_2l";
            }
        }

        std::string candle_rel;
        std::string cl_rel;
        std::string candle_hash;
        std::string cl_hash_expected;
        const auto* files_v = sc_obj.if_contains("market_files");
        if (files_v == nullptr || !files_v->is_array() || files_v->as_array().empty()) {
            throw std::runtime_error(
                "Scenario '" + sc_id + "' requires a non-empty market_files array"
            );
        }
        std::unordered_set<std::string> attested_paths;
        for (const auto& file_v : files_v->as_array()) {
            if (!file_v.is_object()) {
                throw std::runtime_error("Scenario '" + sc_id + "' market_files entries must be objects");
            }
            const auto& file_obj = file_v.as_object();
            arb::pools::reject_unknown_fields(
                file_obj, {"kind", "path", "sha256"}, "market file"
            );
            const std::string kind = arb::get_string_opt(file_obj, "kind", "market");
            const std::string path = arb::get_string_opt(file_obj, "path", "");
            const std::string digest = arb::get_string_opt(file_obj, "sha256", "");
            if (path.empty() || !is_sha256(digest)) {
                throw std::runtime_error(
                    "Scenario '" + sc_id + "' market_files require path and SHA-256"
                );
            }
            if (!attested_paths.insert(path).second) {
                throw std::runtime_error("Scenario '" + sc_id + "' has duplicate market file: " + path);
            }
            if (kind == "market") {
                if (!candle_hash.empty()) {
                    throw std::runtime_error("Scenario '" + sc_id + "' has multiple market candle files");
                }
                candle_rel = path;
                candle_hash = digest;
            } else if (kind == "chainlink") {
                if (!cl_hash_expected.empty()) {
                    throw std::runtime_error("Scenario '" + sc_id + "' has multiple Chainlink files");
                }
                cl_rel = path;
                cl_hash_expected = digest;
            } else {
                throw std::runtime_error(
                    "Scenario '" + sc_id + "' has unsupported market file kind: " + kind
                );
            }
        }
        if (candle_rel.empty() || candle_hash.empty()) {
            throw std::runtime_error("Scenario '" + sc_id + "' candle path is not hash-attested");
        }

        std::string candle_abs = resolve_manifest_path(manifest_dir, candle_rel);
        if (!fs::exists(candle_abs)) {
            throw std::runtime_error("Candles file not found for scenario '" + sc_id + "': " + candle_abs);
        }
        const std::string actual_candle_hash = arb::core::sha256_file(candle_abs);
        if (actual_candle_hash != candle_hash) {
            throw std::runtime_error(
                "Candle SHA-256 mismatch for scenario '" + sc_id + "': expected " +
                candle_hash + ", got " + actual_candle_hash
            );
        }

        uint64_t scen_start_ts = opts.start_ts > 0 ? opts.start_ts : arb::get_u64_opt(sc_obj, "start_time", 0);
        uint64_t scen_end_ts = opts.end_ts > 0 ? opts.end_ts : arb::get_u64_opt(sc_obj, "end_time", 0);
        size_t scen_max_candles = opts.max_candles > 0 ? opts.max_candles : static_cast<size_t>(arb::get_u64_opt(sc_obj, "n_candles", 0));
        double scen_filter_pct = opts.candle_filter_pct > 0.0 ? opts.candle_filter_pct : arb::get_double_opt(sc_obj, "candle_filter", 99.9);
        double filter_squeeze = scen_filter_pct > 0.0 ? (scen_filter_pct / 100.0) : 0.999;
        auto candles = arb::load_candles(candle_abs, scen_max_candles, filter_squeeze, scen_start_ts);
        if (scen_end_ts > 0) {
            candles.erase(
                std::remove_if(candles.begin(), candles.end(), [=](const arb::Candle& candle) {
                    return candle.ts > scen_end_ts;
                }),
                candles.end()
            );
        }
        if (candles.empty()) {
            throw std::runtime_error("No candles loaded for scenario '" + sc_id + "' from " + candle_abs);
        }

        auto events = arb::gen_events(candles);

        // Chainlink feed handling
        std::string cl_abs;
        std::string cl_hash = "none";
        if (!cl_rel.empty()) {
            if (cl_hash_expected.empty()) {
                throw std::runtime_error("Scenario '" + sc_id + "' Chainlink path is not hash-attested");
            }
            cl_abs = resolve_manifest_path(manifest_dir, cl_rel);
            if (!fs::exists(cl_abs) || fs::is_directory(cl_abs)) {
                throw std::runtime_error("Chainlink file not found for scenario '" + sc_id + "': " + cl_abs);
            }
            cl_hash = arb::core::sha256_file(cl_abs);
            if (cl_hash != cl_hash_expected) {
                throw std::runtime_error(
                    "Chainlink SHA-256 mismatch for scenario '" + sc_id + "': expected " +
                    cl_hash_expected + ", got " + cl_hash
                );
            }
            auto cl_points = arb::oracles::load_chainlink_csv(cl_abs);
            arb::oracles::attach_chainlink_prices(events, cl_points);
        }

        auto ev_soa = arb::EventSoA::from_events(events);


        Scenario<T> scen;
        scen.id = sc_id;
        scen.candle_path = candle_abs;
        scen.chainlink_path = cl_abs;
        scen.candles = candles;
        scen.events = std::move(ev_soa);
        scen.base_pool = base_pool;
        scen.base_costs = base_costs;
        scen.start_ts = candles.empty() ? 0 : candles.front().ts;
        scen.yb_mode = scen_yb_mode;

        // Content-based scenario digest: admitted market bytes, parsed candles,
        // and the scenario-declared YieldBasis mode.
        std::ostringstream sc_oss;
        sc_oss << scen.id << "|raw:" << actual_candle_hash << "|parsed:"
               << arb::crypto::sha256_hex(
                      candles.data(), candles.size() * sizeof(arb::Candle))
               << "|" << candles.size() << "|cl:" << cl_hash
               << "|yb:" << scen_yb_mode;
        scen.scenario_sha256 = arb::crypto::sha256_hex(sc_oss.str());

        scenarios_.push_back(std::move(scen));
    }
}

template <typename T>
std::string ScenarioStore<T>::compute_scenario_set_sha256() const {
    std::ostringstream oss;
    for (const auto& sc : scenarios_) {
        oss << sc.id << ":" << sc.scenario_sha256 << ";";
    }
    return arb::crypto::sha256_hex(oss.str());
}

template <typename T>
std::string ScenarioStore<T>::compute_session_fingerprint(
    const std::string& binary_sha256,
    const std::string& policy_source_sha256,
    const std::string& session_config_sha256
) const {
    std::ostringstream oss;
    oss << "bin:" << binary_sha256
        << "|policy:" << policy_source_sha256
        << "|template:" << template_sha256_
        << "|scenarios:" << compute_scenario_set_sha256()
        << "|session_config:" << session_config_sha256;
    return arb::crypto::sha256_hex(oss.str());
}

// Explicit template instantiations
template class ScenarioStore<double>;
template class ScenarioStore<long double>;
template class ScenarioStore<float>;

} // namespace curve_fx::evaluator
