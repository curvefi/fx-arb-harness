# Protocol `curve_fx_eval_v1` Specification

## 1. Overview

`curve_fx_eval_v1` is a line-delimited JSON (NDJSON) streaming protocol between simulation coordinators (e.g. `curve-fx-optimization`, grid search runners, TMRBCD optimizers, shiftclick replay tools) and the high-performance evaluator executable (`arb_evaluator_ld`).

Stdout is strictly reserved for protocol NDJSON frames. All logging, progress updates, and diagnostic messages MUST be directed to stderr.

### Key Tenets
1. **Immutable Session**: Scenarios, candle data, oracle feeds, and baseline pool templates load once and are attested by cryptographic SHA-256 digests.
2. **Deterministic & Ordered**: Result frames match the input candidate batch ordering by ordinal.
3. **Observation Separation**: Observation level (`summary` vs `full_trace`) does not alter economic state machines, random seeds, or metric outputs. Both share the exact same economic simulation loop and produce identical economic fingerprints.
4. **Confined Atomic Sidecars**: Trace artifacts and detailed logs are published atomically within run-relative directories with strict rejection of path traversal (`..` or absolute paths).
5. **Raw Metrics Only**: The evaluator returns raw mathematical metrics without synthesizing optimization scores or penalty losses. All objective functions and penalty modeling remain client-side.
6. **Explicit Backpressure**: The server advertises its capacity limits (frame size, batch size, in-flight units) and enforces one admitted batch at a time.

---

## 2. Frame Format

Every frame transmitted over stdin/stdout is a single-line UTF-8 JSON object terminated with `\n`. Max frame size is bounded (default 4 MiB / 4,194,304 bytes).

### Top-Level Envelope Fields
- `protocol`: String `"curve_fx_eval_v1"` (REQUIRED in all messages)
- `type`: Message type identifier (REQUIRED)
- `request_id`: Correlating request identifier string (REQUIRED on request/response pairs)
- `session_id`: Session identifier string (REQUIRED on session and evaluation operations)

---

## 3. Protocol Lifecycle

### 3.1 Startup & Hello
Upon launching in `serve` mode (or when invoked with `--identity-json`), the evaluator outputs a `hello` frame:

```json
{
  "protocol": "curve_fx_eval_v1",
  "type": "hello",
  "version": 1,
  "evaluator_identity": {
    "binary_sha256": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
    "harness_version": "1.0.0",
    "pool_version": "1.0.0",
    "policy_id": "twocrypto_native",
    "policy_source_sha256": "none",
    "policy_abi": "twocrypto_policy_v1",
    "policy_parameter_count": 0,
    "numeric_mode": "longdouble",
    "real_type": "long double",
    "compiler": "clang-17.0.6",
    "build_target": "arb_evaluator_ld",
    "ipo_enabled": false,
    "native_tuning": false
  },
  "capabilities": ["summary", "full_trace", "atomic_sidecars"],
  "yb_modes": ["off", "passive", "active_2l"],
  "metric_schema": "twocrypto-summary-v1",
  "metric_fields": [
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
    "fee_capture_rate", "donations", "donation_coin0_total", "avg_imbalance", "tvl_growth",
    "keeper_successful_submissions", "fixed_keeper_ticks"
  ],
  "limits": {
    "max_frame_bytes": 4194304,
    "max_inflight_batches": 1
  }
}
```

### 3.2 Session Initialization: `open_session`
The client sends an `open_session` frame with file paths and expected SHA-256 hashes. The manifest is the single authority for market files and has one accepted envelope: `schema_version = fxsim_manifest_v1`, `run_kind = session`, a non-empty `run_id`, and `resolved_spec.scenario`. The scenario contains only `id`, optional candle bounds/filter, a non-empty `market_files` array of `{path, kind, sha256}` objects (where `kind` is `market` or `chainlink`), and optional `yb_mode`/`yb_releverage` declarations. The evaluator rejects unknown fields and validates the template, manifest, candle, and optional Chainlink hashes before admitting the session.

`yb_mode` selects the YieldBasis model and must be one of:
- `"off"` — no YieldBasis: the yb metric family stays empty.
- `"passive"` — metrics-only observer projection: the evaluator re-runs the exact active 2L transition (`try_fire`) in a private second simulation over a copied initial pool, and reports only the yb metric family (`yb_enabled`, `yb_fee`, `yb_apy`, `yb_apy_gm`, `yb_final_growth`, `yb_releverage_trades`, the gm-window and growth-concentration metrics). The primary pool, donation schedule, and actor state are never mutated; all non-YB metrics describe the untouched primary simulation. Because it executes a full second simulation, `passive` roughly doubles evaluation cost and is opt-in.
- `"active_2l"` — the state-mutating 2L contract model (the original YieldBasis path).

The legacy boolean `yb_releverage` remains accepted: `true` maps to `"active_2l"` and is consulted only when `yb_mode` is absent. `yb_releverage_fee` and `yb_cash_multiplier` configure whichever mode is on.

```json
{
  "protocol": "curve_fx_eval_v1",
  "type": "open_session",
  "request_id": "req-init-001",
  "session_id": "session-btc-usd-2024",
  "template_path": "templates/pool_btc_template.json",
  "template_sha256": "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08",
  "manifest_path": "manifests/scenarios_btc.json",
  "manifest_sha256": "5e884898da28047151d0e56f8dc6292773603d0d6aabbdd62a11ef721d1542d8",
  "pool_index": 0,
  "n_candles": 0,
  "start_time": 0,
  "end_time": 0,
  "candle_filter": 0.0,
  "min_swap": 1e-6,
  "max_swap": 1.0,
  "dustswap_freq_s": 3600,
  "dustswap_random": false,
  "dustswap_dynamic_freq_s": 0,
  "dustswap_dynamic_gap_enabled": false,
  "dustswap_dynamic_gap_bps": 0.0,
  "dustswap_dynamic_heartbeat_s": 0,
  "dustswap_commit_clock_freq_s": 0,
  "policy_keeper_enabled": false,
  "allow_hybrid_keeper": false,
  "user_swap_freq_s": 0,
  "user_swap_size_frac": 0.01,
  "user_swap_thresh": 0.05,
  "disable_slippage_probes": false,
  "yb_mode": "off",
  "yb_releverage_fee": 0.012,
  "yb_cash_multiplier": 1.0
}
```

Response (`session_ready`):
```json
{
  "protocol": "curve_fx_eval_v1",
  "type": "session_ready",
  "request_id": "req-init-001",
  "session_id": "session-btc-usd-2024",
  "scenarios": [
    {
      "id": "scenario_01",
      "events_count": 14400,
      "candles_count": 1440,
      "start_ts": 1704067200,
      "end_ts": 1704153540
    }
  ],
  "scenario_set_sha256": "c8f278a0c8f278a0c8f278a0c8f278a0c8f278a0c8f278a0c8f278a0c8f278a0",
  "session_config_sha256": "d2c411f0d2c411f0d2c411f0d2c411f0d2c411f0d2c411f0d2c411f0d2c411f0",
  "session_fingerprint": "a3b199c0a3b199c0a3b199c0a3b199c0a3b199c0a3b199c0a3b199c0a3b199c0",
  "metric_schema_sha256": "f1e255b0f1e255b0f1e255b0f1e255b0f1e255b0f1e255b0f1e255b0f1e255b0"
}
```

### 3.3 Batch Evaluation: `evaluate_batch`

`metric_projection` is a required top-level request field and is independent of trace capture. Clients MUST send either `"summary"` or `"full"`; omission is rejected.

- `summary`: return aggregate `metrics` and `metrics_vec`; `scenario_results` is an empty array.
- `full`: return the same aggregate metrics plus ordered per-scenario `scenario_results`.

It MUST NOT appear inside `observation`; `observation.kind` only selects trace capture (`summary` or `full_trace`).

```json
{
  "protocol": "curve_fx_eval_v1",
  "type": "evaluate_batch",
  "request_id": "batch-101",
  "session_id": "session-btc-usd-2024",
  "metric_projection": "summary",
  "observation": {
    "kind": "summary",
    "trace_interval": 1,
    "trace_actions": false,
    "artifact_dir": "runs/batch-101"
  },
  "candidates": [
    {
      "ordinal": 0,
      "candidate_id": "cand-0",
      "policy_params": [3600.0, 86400.0, 1.0, 10.0, 0.0],
      "pool_overrides": {
        "mid_fee": 0.0005,
        "out_fee": 0.0040,
        "fee_gamma": 0.0001
      }
    }
  ]
}
```

Response (`batch_result`) echoes the selected projection:
```json
{
  "protocol": "curve_fx_eval_v1",
  "type": "batch_result",
  "request_id": "batch-101",
  "session_id": "session-btc-usd-2024",
  "status": "complete",
  "metric_projection": "summary",
  "results": [
    {
      "ordinal": 0,
      "candidate_id": "cand-0",
      "status": "ok",
      "economic_fingerprint": "8d9e2a...",
      "metrics": {"vp": 1.0452, "apy_net_gm": 0.158, "trades": 450},
      "metrics_vec": [1.0452, 0.158, 450],
      "scenario_results": [],
      "artifacts": null
    }
  ],
  "elapsed_ms": 45.67
}
```

With `"metric_projection": "full"`, each candidate's `scenario_results` contains all scenario rows in canonical scenario order. Projection changes response detail only; it never changes economic execution or the economic fingerprint.
### 3.4 Full Trace Observation (`observation.kind = "full_trace"`)
When `observation.kind` is `"full_trace"`, a non-empty relative `artifact_dir` is required. Trace records and actions are written to atomic content-addressed files:
- `<artifact_dir>/<economic_fingerprint>.<scenario_id>.<trace_sha256>.trace.json`
- `<artifact_dir>/<economic_fingerprint>.<scenario_id>.<actions_sha256>.actions.json`
- `<artifact_dir>/<economic_fingerprint>.<manifest_sha256>.manifest.json`

The returned `batch_result` includes the artifact relative paths and SHA-256 checksums in the `artifacts` field.

### 3.5 Session Close & Shutdown
- `{"protocol":"curve_fx_eval_v1","type":"close_session","request_id":"close-1","session_id":"session-btc-usd-2024"}` -> `{"protocol":"curve_fx_eval_v1","type":"session_closed","request_id":"close-1","session_id":"session-btc-usd-2024"}`
- `{"protocol":"curve_fx_eval_v1","type":"shutdown","request_id":"shut-1"}` -> process terminates gracefully.

---

## 4. Error Handling

When an operation fails, an `error` frame is emitted:
```json
{
  "protocol": "curve_fx_eval_v1",
  "type": "error",
  "request_id": "req-init-001",
  "scope": "session",
  "error_code": "ATTESTATION_FAILED",
  "message": "SHA-256 digest mismatch for template_path: expected 9f86..., got 1234..."
}
```

Error scopes:
- `protocol`: JSON syntax error, frame limit exceeded, unknown type.
- `session`: Attestation mismatch, file not found, invalid session state.
- `candidate`: Invalid candidate parameters or pool overrides.
- `evaluation`: Simulation runtime error.
- `sidecar`: Path traversal violation or I/O error writing artifacts.
