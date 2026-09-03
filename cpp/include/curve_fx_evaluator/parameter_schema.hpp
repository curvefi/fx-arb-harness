#pragma once

#include <array>
#include <string_view>

namespace curve_fx::evaluator {

struct StaticParameterDescriptor {
    std::string_view name;
    std::string_view lowering_path;
    std::string_view type;
    std::string_view unit;
    std::string_view wire;
    std::string_view classification;
    std::string_view default_json;
    std::string_view choices_json{};
};

inline constexpr std::array<StaticParameterDescriptor, 54> STATIC_PARAMETERS{{
    {"pool.tag", "pool_overrides.tag", "string", "identifier", "utf8", "candidate", ""},
    {"pool.initial_liquidity", "pool_overrides.pool.initial_liquidity", "real_pair", "token_amount", "binary64_from_wad_1e18", "candidate", ""},
    {"pool.A", "pool_overrides.pool.A", "real", "pool_raw", "binary64", "candidate", ""},
    {"pool.gamma", "pool_overrides.pool.gamma", "real", "pool_raw", "binary64", "candidate", ""},
    {"pool.mid_fee", "pool_overrides.pool.mid_fee", "real", "fee_fraction", "binary64_fraction_or_1e10", "candidate", ""},
    {"pool.out_fee", "pool_overrides.pool.out_fee", "real", "fee_fraction", "binary64_fraction_or_1e10", "candidate", ""},
    {"pool.fee_gamma", "pool_overrides.pool.fee_gamma", "real", "ratio", "binary64", "candidate", ""},
    {"pool.adjustment_step_min", "pool_overrides.pool.adjustment_step_min", "real", "ratio", "binary64", "candidate", ""},
    {"pool.adjustment_step_max", "pool_overrides.pool.adjustment_step_max", "real", "ratio", "binary64", "candidate", ""},
    {"pool.ma_time", "pool_overrides.pool.ma_time", "real", "seconds", "binary64", "candidate", ""},
    {"pool.reserved_profit_fraction", "pool_overrides.pool.reserved_profit_fraction", "real", "fee_fraction", "binary64_fraction_or_1e10", "candidate", ""},
    {"pool.admin_fee", "pool_overrides.pool.admin_fee", "real", "fee_fraction", "binary64_fraction_or_1e10", "candidate", ""},
    {"pool.initial_price", "pool_overrides.pool.initial_price", "real", "price", "binary64_from_wad_1e18", "candidate", ""},
    {"pool.start_timestamp", "pool_overrides.pool.start_timestamp", "integer", "unix_seconds", "uint64_or_milliseconds", "candidate", ""},
    {"pool.historical_state", "pool_overrides.pool.historical_state", "object", "pool_state", "json_object_binary64_boundary", "candidate", ""},
    {"pool.donation_apy", "pool_overrides.pool.donation_apy", "real", "annual_fraction", "binary64", "candidate", ""},
    {"pool.donation_frequency", "pool_overrides.pool.donation_frequency", "real", "seconds", "binary64", "candidate", ""},
    {"pool.donation_duration", "pool_overrides.pool.donation_duration", "real", "seconds", "binary64", "candidate", ""},
    {"pool.initial_donation_days", "pool_overrides.pool.initial_donation_days", "real", "days", "binary64", "candidate", ""},
    {"pool.donation_coins_ratio", "pool_overrides.pool.donation_coins_ratio", "real", "ratio", "binary64", "candidate", ""},
    {"pool.user_swap_size_frac", "pool_overrides.pool.user_swap_size_frac", "real", "daily_tvl_fraction", "binary64", "candidate", ""},
    {"pool.costs.arb_fee_bps", "pool_overrides.costs.arb_fee_bps", "real", "basis_points", "binary64", "candidate", ""},
    {"pool.costs.gas_coin0", "pool_overrides.costs.gas_coin0", "real", "coin0", "binary64", "candidate", ""},
    {"pool.costs.use_volume_cap", "pool_overrides.costs.use_volume_cap", "boolean", "flag", "json_boolean", "candidate", ""},
    {"pool.costs.volume_cap_mult", "pool_overrides.costs.volume_cap_mult", "real", "ratio", "binary64", "candidate", ""},
    {"pool.costs.volume_cap_is_coin_1", "pool_overrides.costs.volume_cap_is_coin_1", "boolean", "flag", "json_boolean_or_binary64", "candidate", ""},
    {"pool.run.yb_releverage_fee", "pool_overrides.run.yb_releverage_fee", "real", "fee_fraction", "binary64", "candidate", ""},
    {"run.session_id", "open_session.session_id", "string", "identifier", "utf8", "session", ""},
    {"run.template_path", "open_session.template_path", "string", "path", "utf8", "session", ""},
    {"run.scenario_id", "open_session.scenario_id", "string", "identifier", "utf8", "session", ""},
    {"run.market_path", "open_session.market_path", "string", "path", "utf8", "session", ""},
    {"run.price_feed_path", "open_session.price_feed_path", "string", "path", "utf8", "session", ""},
    {"run.pool_index", "open_session.pool_index", "integer", "index", "uint64", "session", "0"},
    {"run.n_candles", "open_session.n_candles", "integer", "count", "uint64", "session", "0"},
    {"run.start_time", "open_session.start_time", "integer", "unix_seconds", "uint64", "session", "0"},
    {"run.end_time", "open_session.end_time", "integer", "unix_seconds", "uint64", "session", "0"},
    {"run.candle_filter", "open_session.candle_filter", "real", "percent", "binary64", "session", "0.0"},
    {"run.min_swap", "open_session.min_swap", "real", "ratio", "binary64", "session", "1e-6"},
    {"run.max_swap", "open_session.max_swap", "real", "ratio", "binary64", "session", "1.0"},
    {"run.dustswap_freq_s", "open_session.dustswap_freq_s", "integer", "seconds", "uint64", "session", "3600"},
    {"run.user_swap_freq_s", "open_session.user_swap_freq_s", "integer", "seconds", "uint64", "session", "0"},
    {"run.user_swap_size_frac", "open_session.user_swap_size_frac", "real", "daily_tvl_fraction", "binary64", "session", "0.01"},
    {"run.user_swap_thresh", "open_session.user_swap_thresh", "real", "ratio", "binary64", "session", "0.05"},
    {"run.enable_slippage_probes", "open_session.enable_slippage_probes", "boolean", "flag", "json_boolean", "observation", "false"},
    {"run.event_cursor", "open_session.event_cursor", "enum", "event_cursor", "utf8", "session", "\"scalar\"", "[\"scalar\",\"exact_skip\"]"},
    {"run.metric_profile", "open_session.metric_profile", "enum", "metric_profile", "utf8", "session", "\"full_summary\"", "[\"full_summary\",\"grid_core\"]"},
    {"run.yb_mode", "open_session.yb_mode", "enum", "yb_mode", "utf8", "session", "\"off\"", "[\"off\",\"active_2l\",\"reference_2l\"]"},
    {"run.yb_releverage_fee", "open_session.yb_releverage_fee", "real", "fee_fraction", "binary64", "session", "0.012"},
    {"run.yb_cash_multiplier", "open_session.yb_cash_multiplier", "real", "ratio", "binary64", "session", "1.0"},
    {"run.metric_projection", "evaluate_batch.metric_projection", "enum", "projection", "utf8", "observation", "", "[\"summary\",\"full\"]"},
    {"run.observation.kind", "evaluate_batch.observation.kind", "enum", "observation_kind", "utf8", "observation", "\"summary\"", "[\"summary\",\"full_trace\"]"},
    {"run.observation.trace_interval", "evaluate_batch.observation.trace_interval", "integer", "events", "uint64", "observation", "1"},
    {"run.observation.trace_actions", "evaluate_batch.observation.trace_actions", "boolean", "flag", "json_boolean", "observation", "false"},
    {"run.observation.artifact_dir", "evaluate_batch.observation.artifact_dir", "string", "relative_path", "utf8", "observation", ""},
}};

} // namespace curve_fx::evaluator
