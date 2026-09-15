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
    "policy_id": "none",
    "policy_abi": "none",
    "policy_parameter_count": 0,
    "numeric_mode": "longdouble",
    "real_type": "long double",
    "compiler": "clang",
    "build_target": "arb_evaluator_ld",
    "ipo_enabled": false,
    "native_tuning": false
  },
  "capabilities": ["summary", "full_trace", "atomic_sidecars",
                   "registered_grid_ranges", "state_reconciliation"],
  "yb_modes": ["off", "active_2l", "reference_2l"],
  "metric_schema": "twocrypto-summary-v1",
  "metric_fields": [
    "vp", "lp_xcp_profit", "apy", "apy_net", "apy_net_gm",
    "apy_net_robust_90d", "avg_rel_price_diff", "max_rel_price_diff",
    "max_7d_rel_price_diff", "final_rel_price_diff", "detach_energy_ungated",
    "avg_imbalance", "tw_avg_pool_fee", "min_pool_fee", "max_pool_fee",
    "tw_real_slippage_1pct", "tw_real_slippage_5pct",
    "tw_real_slippage_10pct", "trades", "n_rebalances",
    "arb_guarded_loss_coin0", "yb_apy", "yb_apy_gm", "yb_final_growth",
    "yb_fee", "yb_releverage_trades", "yb_gm_windows",
    "yb_gm_floored_windows", "yb_gm_floor_share", "elapsed_ms",
    "total_notional_coin0", "lp_fee_coin0", "arb_pnl_coin0",
    "fee_capture_rate", "donations", "donation_coin0_total", "tvl_growth"
  ],
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
  "price_feed_path": "data/eurusd-reference-prices.csv",
  "cex_depth_path": "data/eurusd-depth.npz",
  "cex_depth_max_age_s": 30,
  "actor_timing_mode": "legacy_event",
  "event_mode": "depth",
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
  "yb_cash_multiplier": 1.0,
  "yb_initial_state": null
}
```

The remaining optional session controls are `user_swap_freq_s`,
`user_swap_size_frac`, `user_swap_thresh`, `event_cursor`, `metric_profile`,
and `enable_slippage_probes`. Slippage probes are off unless explicitly enabled.
`event_mode="candle_path"` retains the two synthetic `ts-5`/`ts+5` events per
candle and requires `market_path`. `event_mode="depth"` requires `cex_depth_path`
and omits `market_path`; `candle_filter` must be zero. Its regular clock starts at
the later of `start_time` and the first whole second at or after the first book
publication. Events repeat every `observation_interval_s` (default 60), through
the earlier of `end_time` (if supplied) and the first whole second at or after
the final publication. `n_candles` caps observation count in this mode; at most
2678400 observations may be admitted. Prices are the latest published book
midpoint, carried across gaps, with zero synthetic volume. Execution still
requires a fresh book. The trace adapter uses internal flat rows, with no
companion OHLC file. An empty requested window is rejected.
`user_swap_size_frac` is the daily fair-TVL utilization fraction: each scheduled
order has coin0-equivalent notional `fair_tvl * user_swap_size_frac *
user_swap_freq_s / 86400`, converted into the alternating input coin at the
current external price. Thus `1.0` means 100% attempted fair-TVL turnover per
day, not 100% of one reserve per swap.
Native arbitrage sizing and execution are always evaluated. Model weak or absent
native arbitrage economically with `pool.costs.arb_fee_bps`, gas, and volume caps.
These native execution costs are not charged to either YB actor; protocol/YB
fees remain part of their existing models.
`event_cursor=exact_skip` requires `metric_profile=grid_core`. Scalar supports
both metric profiles and remains the reference cursor.
`yb_mode` is `off`, `active_2l`, or `reference_2l`.
The enabled modes use `yb_releverage_fee` and `yb_cash_multiplier`, and evaluate
after every causal event. Summary valuation is hourly for GM accounting and once
at the final endpoint for raw APY.

`cex_depth_path` accepts only `.npz`. The canonical v2 archive has exactly these
C-order numeric arrays:

| Array | Type and shape | Meaning |
| --- | --- | --- |
| `format_version` | little-endian uint32 scalar | `2` |
| `depth` | little-endian float64 `[N,2,K,2]` | bid/ask, price/incremental coin1 quantity |
| `timestamps` | little-endian int64 `[N]` | positive, strictly increasing publication UTC nanoseconds |
| `counts` | little-endian uint32 `[N,2]` | active levels per side, from 1 through K |
| `interpolation` | uint8 `[N]` | 0 step, 1 linear, per snapshot |

Publication intervals may be irregular. K is 1–4096, N is at most 267840,
and `depth` is at most 17141760 float64 elements (137134080 bytes).
Inactive padding must be zero. Prices are positive and ordered, bids descending
and asks ascending; the best bid cannot exceed the best ask. Quantities are
positive except zero-quantity interior linear knots. Locked books are supported.
The reader also accepts existing v1 archives unchanged: K=16, uint8 counts,
exact 10-second clock, strictly uncrossed books, implicit step interpolation.
The producer writes v2. ZIP/DEFLATE and CRCs use MiniZip; object/pickle arrays,
unknown members, and duplicate members are rejected. No currency conversion
is performed: price is coin0 per coin1. Units and reconstruction provenance
belong in the associated manifest.

JSONL is an upstream conversion/inspection format. From the sibling `data/cryptolake` project,
convert to a **new** destination with:

```sh
uv run python -m cryptolake.depth_archive source.jsonl book.npz --quantity-base BTC --price-quote USDT
```

This preserves publication times, float64 values, active levels, and per-row
step/linear semantics without resampling. It writes `book.npz.json` with units
and a source hash; other source metadata remains in the preserved JSONL and
its original manifests. Conversion does not certify causal reconstruction.
Existing sources, archives, and output paths are never overwritten by this
converter. Large sources must be split into archives within the stated limits.
Python can inspect the same NPZ using `np.load(..., allow_pickle=False)`.

The loop selects only
the latest snapshot published by the event timestamp, uses its top-of-book
midpoint as the market mark, and shares depletion across native actions until
a newer snapshot arrives. `cex_depth_max_age_s` is a non-negative uint64
(default 30); zero accepts only an exact publication-time match. Future,
missing, stale, or exhausted depth prevents external execution without an
infinite-liquidity fallback. Synthetic user flow never consumes the tape, and
the independent policy price feed is unchanged. Depth supports all three YB
modes. In `legacy_event`, `reference_2l` shares the depleted book after native
execution. `active_2l` retains its established midpoint-based LP model: finite
depth constrains native arbitrage, but the active actor does not execute or
consume a CEX depth hedge. It requires a fresh book when depth is configured.
Configured `exact_skip` uses the scalar cursor so snapshots cannot be skipped.

The per-row `interpolation` array selects step or linear depth.
Pairs remain `[price, incremental_quantity]`; cumulative knot quantities are
their prefix sums. The first level retains its exact constant best price.
Subsequent levels interpolate marginal price linearly from the previous knot's
price to the current price across that level's quantity. Quotes integrate this
line, including partial fills, and both native and VP sizing use the same quote
implementation. Zero-quantity interior knots represent price gaps and add no
liquidity. The first quantity must be positive. Queries past total capacity fail.
Thus knots at +200 bp/100 BTC and +500 bp/200 BTC imply +350 bp at 150 BTC;
the total cost is the integral, not 150 BTC multiplied by that marginal price.

`actor_timing_mode="minute_sequential"` runs exactly one native arbitrage
decision followed by one selected YB decision (`reference_2l` or `active_2l`)
at each selected observation.
`observation_interval_s` defaults to 60 and accepts any positive integer number
of seconds, independently of publication cadence. Observations use the depth
clock described above; each observation admits actors and may produce a trace
row. Books are selected causally at both observation and execution time.
At each selected time, the newest causal depth is used; this setting changes
actor cadence, not the underlying tape or a replenishment-rate model.
Either actor may decline an unprofitable trade. Both YB modes see the native
trade's updated pool state. With `reference_2l`, native and VP each quote and
execute against independent copies of the current snapshot, freshly copied
at every observation. Native consumption does not reduce VP or subsequent
observation depth. With `active_2l`, only native arbitrage consumes a copy;
YB keeps its midpoint-based model. Missing or stale depth disables both actors.
Pool and YB state carry across observations; book snapshots are not interpolated
in time.
Use `event_mode="depth"`, `event_cursor="scalar"`,
`metric_profile="full_summary"`, `dustswap_freq_s=0`, `user_swap_freq_s=0`,
`yb_mode="reference_2l"` or `"active_2l"`, finite depth and no volume cap.
Execution is immediate at each selected observation timestamp.

`observed_state_path` optionally supplies complete coupled pool/YB checkpoints
for `minute_sequential` with `yb_mode="reference_2l"` and no pool policy.
Observed checkpoints and resets are not supported by `active_2l`. JSONL rows strictly increase by
source block and availability. Required fields are
`available_ns=(source_timestamp+1)*1e9`, `source_block`, `source_timestamp`,
`observed_through_timestamp=source_timestamp`, `pool_init` (standard inner pool
object with complete historical state in WAD units), `yb_initial_state`
(existing human-unit schema), and constant `coverage_start_timestamp` and
`coverage_end_timestamp`. Native/YB provenance must match the row. Additional
fields in older checkpoint files are ignored.

`state_reconciliation_mode` supports `off` (default) and `on_price_scale_detach`.
After both actors, the latter checks absolute simulated minus latest published
onchain `price_scale`, divided by the onchain scale. At or above
`reset_threshold_bps` (finite positive, default 100), it pauses both actors and
latches a deadline at detection plus `equalization_delay_s` (nonnegative, default
60). Later price recovery or observations do not extend or cancel that request.
At the first selected observation at or after the deadline, before either actor,
it copies the latest causally available coupled checkpoint and resumes trading.
The delay may be zero; because detection follows execution, application still
occurs at the next selected observation. `off` loads observations without resets.

Restoration includes native stored clocks, EMA inputs, donation/admin state and
configuration, and YB debt/rate/cash/collateral/stable-aggregator state. Stored
clocks come from the checkpoint; execution time advances to the current observation.
Equal public state closes the request without a copy. YB simulated interest
counters retain their accrued/donated history with a rebased public stock.
`state_reconciliation` trace actions use `request`, `apply`, and `equal`, with
detection/deadline/application times, checkpoint provenance, reset segment, and
before/after corrections. `actor_metrics.state_reconciliation` contains only
observation, episode, and reset counts plus the accounting qualification.
Pool/LP/YB valuation metrics across copied boundaries are comparison-only:
copied public state is not simulated profit.

`yb_min_net_profit_coin0` is a finite nonnegative session setting, default 1.0.
Only `reference_2l` admission uses it: candidate profit after all existing external
charges must strictly exceed this extra coin0 margin. Zero permits strictly
positive net profit. It changes neither transaction gas, protocol/AMM fees, sizing,
nor accounting. Full-trace effective inputs expose `run.yb_min_net_profit_coin0`.

`yb_initial_state` optionally replaces synthetic fresh-2L initialization. It is
a complete object with provenance fields `source_block`, `source_timestamp`, and
`block_hash`; common fields `leverage`, `fee`, `collateral`, `debt`, `rate`,
`rate_mul`, `rate_time`, `minted`, `redeemed`, `stable_balance`,
`lt_stable_balance`, and `killed`; and reference fields `flash_max_loan`,
`stable_aggregator`, `rounding_discount`, and `lt_donation_discount`. Quantities
are human-unit finite binary64 values. `debt` and `rate_mul` are the stored AMM
values at `rate_time`, rather than projected getters. When the checkpoint is
applied, its source block and timestamp must equal the native pool
`historical_state`; `rate_time` may not be later than the source timestamp.
Leverage is fixed at 2 and safety ratios are derived from it. Stable cash and
flash capacity may be zero.

For enabled modes, the historical `fee` is authoritative. An explicitly supplied
conflicting `yb_releverage_fee`, including a candidate override, is rejected; an
omitted raw-protocol fee inherits the checkpoint value. `yb_cash_multiplier` and
donation-APY rate derivation apply only to fresh initialization. In `off` mode a
retained state object is validated but ignored. Reference external inputs seed
only the initial state; without a chronological update tape this is represented state replay, not full E0b parity.

```json
{
  "protocol": "curve_fx_eval",
  "type": "session_ready",
  "request_id": "open-1",
  "session_id": "session-1",
  "scenario": {"id": "scenario-1", "events_count": 14400,
                 "candles_count": 1440, "start_ts": 1704067200,
                 "end_ts": 1704153540}
}
```

`scenario_id` and `template_path` are required. `candle_path` requires
`market_path`; `depth` requires `cex_depth_path` and omits `market_path`.
`price_feed_path` and `observed_state_path` are optional subject to the mode
constraints above. The response contains one `scenario` object with event and
candle/observation counts.

### `evaluate_batch`

`metrics_format` is `object` or `array`, and defaults to `object`.
`metric_fields` optionally selects a non-empty, unique, order-significant subset
of the canonical fields advertised by `hello`; array format requires it.
Candidates carry a unique `ordinal`, a unique `candidate_id`, finite binary64
`policy_params`, and optional `pool_overrides`. Results are sorted by ordinal.

```json
{
  "protocol": "curve_fx_eval", "type": "evaluate_batch",
  "request_id": "batch-1", "session_id": "session-1",
  "observation": {"kind": "summary"},
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
  "metrics_format": "array",
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
Successful synthetic user swaps appear in that sidecar as `type="exchange"`
with `actor="user"`; their `dx`, `dy_after_fee`, and `fee_tokens` are the values
returned by the committed pool exchange. Failed attempts emit no action. Existing
exchange entries without `actor` are arbitrage fills and retain `profit_coin0`.
Summary `trades`, notional, LP-fee, and arbitrage-PnL metrics remain arbitrage-only;
`n_rebalances` counts price-scale movement from both arbitrage and user fills.

```json
{
  "protocol": "curve_fx_eval", "type": "batch_result",
  "request_id": "batch-1", "session_id": "session-1", "status": "complete",
  "results": [{"ordinal": 0, "candidate_id": "candidate-0", "status": "ok",
               "metrics": {"vp": 0.0, "apy": 0.0, "trades": 0.0},
               "artifacts": null}],
  "elapsed_ms": 1.0
}
```

With `metrics_format = array`, the response echoes the exact requested
`metric_fields` once at batch level and each candidate `metrics` array has the
same length and order. Object and array forms carry identical values.

With `full_trace`, successful results return an
`artifacts` object containing `trace_path`, optional `actions_path`, and
`effective_inputs`. It contains resolved pool and run controls; an applied YB
checkpoint appears as nested `run.yb_initial_state`, including its provenance.
Candidate policy parameters remain in the candidate payload.

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
 "message":"evaluate_batch requires candidates or grid ranges","details":{}}
```

Unknown fields are rejected. Invalid JSON, oversized frames, missing paths,
invalid direct inputs, non-finite numeric inputs, duplicate candidate IDs/ordinals,
and session mismatches are reported as errors without changing the active
session.
