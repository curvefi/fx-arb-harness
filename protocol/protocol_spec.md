# Protocol `curve_fx_eval` specification

The evaluator is a line-delimited JSON (NDJSON) subprocess. Every frame is one
UTF-8 JSON object followed by `\n`; stdout is protocol-only and diagnostics go
to stderr. The maximum frame size is 4 MiB. There is no protocol version field:
the literal `protocol` value is `curve_fx_eval` in every frame.

## Lifecycle

`serve` writes `hello`, then accepts one immutable `open_session`, an optional
`register_grid`, one or more `evaluate_batch` requests, `close_session`, and
`shutdown`. A process admits at most one session and one registered grid; a new
evaluator process is required for another session.

### `hello`

```json
{
  "protocol": "curve_fx_eval",
  "type": "hello",
  "evaluator_identity": {
    "harness_version": "1.0.0",
    "pool_version": "1.0.0",
    "policy_id": "native_passthrough",
    "policy_abi": "twocrypto_policy_v1",
    "policy_parameter_count": 0,
    "numeric_mode": "longdouble",
    "real_type": "long double",
    "compiler": "clang",
    "build_target": "arb_evaluator_ld",
    "ipo_enabled": false,
    "native_tuning": false,
    "pgo_mode": "off"
  },
  "capabilities": ["summary", "full_trace", "atomic_sidecars", "registered_grid_ranges"],
  "yb_modes": ["off", "active_2l", "reference_2l"],
  "metric_schema": "twocrypto-summary-v1",
  "metric_fields": ["vp", "apy", "trades", "yb_enabled", "yb_apy"],
  "limits": {"max_frame_bytes": 4194304,
             "max_candidates_per_batch": 4096,
             "max_metric_values_per_batch": 131072,
             "max_materialized_batch_bytes": 67108864,
             "max_inflight_batches": 1}
}
```

`--identity-json` emits the same identity-shaped record and exits. The separate
`--describe-json` output is an executable-bound description and is not a frame.

### `open_session`

The request carries the evaluator-visible pool template and scenario inputs.
They are loaded once at admission.

```json
{
  "protocol": "curve_fx_eval",
  "type": "open_session",
  "request_id": "open-1",
  "session_id": "session-1",
  "template_path": "templates/pool.json",
  "scenario_id": "eurusd-2024",
  "market_path": "data/eurusd.json",
  "chainlink_path": "data/eurusd-chainlink.csv",
  "pool_index": 0,
  "n_candles": 0,
  "start_time": 0,
  "end_time": 0,
  "candle_filter": 0.0,
  "min_swap": 1e-6,
  "max_swap": 1.0,
  "dustswap_freq_s": 3600,
  "enable_slippage_probes": false,
  "yb_mode": "off",
  "yb_releverage_fee": 0.012,
  "yb_cash_multiplier": 1.0
}
```

The remaining optional session controls are `dustswap_random`,
`dustswap_dynamic_freq_s`, `dustswap_dynamic_gap_enabled`,
`dustswap_dynamic_gap_bps`, `dustswap_dynamic_heartbeat_s`,
`dustswap_commit_clock_freq_s`, `policy_keeper_enabled`, `allow_hybrid_keeper`,
`user_swap_freq_s`, `user_swap_size_frac`, `user_swap_thresh`, and
`enable_slippage_probes`. Slippage probes are off unless explicitly enabled.
`yb_mode` is `off`, `active_2l`, or `reference_2l`.
The enabled modes use `yb_releverage_fee` and `yb_cash_multiplier`, and evaluate
after every causal event. Summary valuation is hourly for GM accounting and once
at the final endpoint for raw APY. `reference_2l` is a floating candidate lane
with synthetic fresh-L2 state, not a historical LT replay.

```json
{
  "protocol": "curve_fx_eval",
  "type": "session_ready",
  "request_id": "open-1",
  "session_id": "session-1",
  "scenarios": [{"id": "scenario-1", "events_count": 14400,
                 "candles_count": 1440, "start_ts": 1704067200,
                 "end_ts": 1704153540}]
}
```

`scenario_id`, `market_path`, and `template_path` are required;
`chainlink_path` is optional. The response keeps `scenarios` as an array so
clients can consume its event and candle counts uniformly.

### `evaluate_batch`

`metric_projection` is required and is either `summary` or `full`.
`metrics_format` is `object` or `array`, and defaults to `object`.
`metric_fields` optionally selects a non-empty, unique, order-significant subset
of the canonical fields advertised by `hello`; array format requires it.
Candidates carry a unique `ordinal`, a unique `candidate_id`, finite binary64
`policy_params`, and optional `pool_overrides`. Results are sorted by ordinal.

```json
{
  "protocol": "curve_fx_eval", "type": "evaluate_batch",
  "request_id": "batch-1", "session_id": "session-1",
  "metric_projection": "summary", "observation": {"kind": "summary"},
  "candidates": [{"ordinal": 0, "candidate_id": "candidate-0",
                  "policy_params": [], "pool_overrides": {}}]
}
```

Exhaustive clients register one immutable grid after opening the session:

```json
{
  "protocol": "curve_fx_eval", "type": "register_grid",
  "request_id": "grid-1", "session_id": "session-1", "grid_id": "grid",
  "candidate_defaults": {"policy_params": [], "pool": {}},
  "axes": {
    "flat_fee": [
      {"pool.mid_fee": 0.005, "pool.out_fee": 0.005},
      {"pool.mid_fee": 0.01, "pool.out_fee": 0.01}
    ],
    "pool.A": [30000, 60000]
  },
  "axis_order": ["flat_fee", "pool.A"],
  "shape": [2, 2]
}
```

The evaluator validates dotted paths and compiles every default and axis value
through the ordinary typed pool-override parser. Every update targets a leaf
below `policy_params` or `pool`; replacing either whole object is invalid.
Subsequent batches carry only ordered, disjoint `[start, count]` ranges:

```json
{
  "protocol": "curve_fx_eval", "type": "evaluate_batch",
  "request_id": "batch-2", "session_id": "session-1",
  "metric_projection": "summary", "metrics_format": "array",
  "metric_fields": ["apy_net", "max_7d_rel_price_diff"],
  "observation": {"kind": "summary"},
  "grid_id": "grid", "ranges": [[0, 1], [3, 1]]
}
```

`axis_order` is explicit and order-significant; `shape` must exactly match the
corresponding non-empty value arrays. Mixed-radix decoding uses C order (the
last axis varies fastest). A scalar axis value updates its dotted axis name; an
object value applies each of its dotted keys, allowing linked axes such as a
flat fee. Empty `axes`, `axis_order`, and `shape` together describe the one
defaults-only candidate. Ranges expand to unique global grid positions and produce
IDs `p00000000`, `p00000001`, and so on. Results preserve request order using
those global ordinals. The input-frame, candidate-count, and metric-cell limits
in `hello` are enforced before the simulation.

`observation.kind` is `summary` or `full_trace`. `trace_interval` is a positive
integer and `trace_actions` controls the optional action sidecar. Observation
changes capture only, not the economic simulation.

```json
{
  "protocol": "curve_fx_eval", "type": "batch_result",
  "request_id": "batch-1", "session_id": "session-1", "status": "complete",
  "metric_projection": "summary",
  "results": [{"ordinal": 0, "candidate_id": "candidate-0", "status": "ok",
               "metrics": {"vp": 0.0, "apy": 0.0, "trades": 0.0},
               "scenario_results": [], "artifacts": null}],
  "elapsed_ms": 1.0
}
```

With `metrics_format = array`, the response echoes the exact requested
`metric_fields` once at batch level and each candidate `metrics` array has the
same length and order. Object and array forms carry identical values.

With `metric_projection = full`, `scenario_results` contains the admitted
scenario's raw metrics. With `full_trace`, successful results return an
`artifacts` object containing `trace_path`, optional `actions_path`, and
`effective_inputs`. The latter is a finite numeric map of the resolved pool
and run controls used to initialize the replay; candidate policy parameters
remain in the candidate payload.

### Close, shutdown, and errors

```json
{"protocol":"curve_fx_eval","type":"close_session","request_id":"close-1","session_id":"session-1"}
```

The response is `session_closed` with the same `request_id` and session ID.
`shutdown` has only `protocol`, `type`, and `request_id`; the process exits
successfully after flushing prior frames.

Errors retain the request ID where available:

```json
{"protocol":"curve_fx_eval","type":"error","request_id":"batch-1",
 "scope":"protocol","error_code":"MISSING_REQUIRED_FIELD",
 "message":"evaluate_batch requires metric_projection","details":{}}
```

Unknown fields are rejected. Invalid JSON, oversized frames, missing paths,
invalid direct inputs, non-finite numeric inputs, duplicate candidate IDs/ordinals,
and session mismatches are reported as errors without changing the active
session.
