# Protocol `curve_fx_eval` specification

The evaluator is a line-delimited JSON (NDJSON) subprocess. Every frame is one
UTF-8 JSON object followed by `\n`; stdout is protocol-only and diagnostics go
to stderr. The maximum frame size is 4 MiB. There is no protocol version field:
the literal `protocol` value is `curve_fx_eval` in every frame.

## Lifecycle

`serve` writes `hello`, then accepts one immutable `open_session`, one or more
`evaluate_batch` requests, `close_session`, and `shutdown`. A process admits at
most one session; a new evaluator process is required for another session.

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
    "native_tuning": false
  },
  "capabilities": ["summary", "full_trace", "atomic_sidecars"],
  "yb_modes": ["off", "passive", "active_2l"],
  "metric_schema": "twocrypto-summary-v1",
  "metric_fields": ["vp", "apy", "trades", "yb_enabled", "yb_apy"],
  "limits": {"max_frame_bytes": 4194304, "max_inflight_batches": 1}
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
`disable_slippage_probes`. `yb_mode` is `off`, `passive`, or `active_2l`;

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

`metric_projection` is required and is either `summary` or `full`. Candidates
carry a unique `ordinal`, a unique `candidate_id`, finite binary64
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

With `metric_projection = full`, `scenario_results` contains the admitted
scenario's raw metrics. With `full_trace`, successful results return
`trace_path` and, when requested, `actions_path`.

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
