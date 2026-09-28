# Protocol `curve_fx_eval` specification

The evaluator is a line-delimited JSON (NDJSON) subprocess. Every frame is one
UTF-8 JSON object followed by `\n`; stdout is protocol-only and diagnostics go
to stderr. There is no fixed frame-size cap or protocol version field:
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
                   "registered_grid_ranges", "maker_trade_flow"],
  "yb_modes": ["off", "active_2l", "reference_2l"],
  "metric_schema": "twocrypto-summary-v1",
  "metric_fields": [
    "vp", "lp_xcp_profit", "apy", "apy_net", "apy_net_gm",
    "apy_net_robust_90d", "avg_rel_price_diff", "max_rel_price_diff",
    "max_7d_rel_price_diff", "final_rel_price_diff", "detach_energy_ungated",
    "avg_imbalance", "tw_avg_pool_fee", "min_pool_fee", "max_pool_fee",
    "tw_real_slippage_1pct", "tw_real_slippage_5pct",
    "tw_real_slippage_10pct", "trades", "n_rebalances",
    "arb_guarded_loss_coin0", "yb_apy", "yb_apy_gm", "yb_gm30", "yb_gm60", "yb_gm90", "yb_final_growth",
    "yb_fee", "yb_releverage_trades", "yb_round_trips", "yb_gm_windows",
    "yb_gm_floored_windows", "yb_gm_floor_share", "elapsed_ms",
    "total_notional_coin0", "lp_fee_coin0", "arb_pnl_coin0",
    "fee_capture_rate", "donations", "donation_coin0_total", "tvl_growth"
  ],
  "limits": {"max_inflight_batches": 1}
}
```

`--identity-json` emits the same identity-shaped record and exits. The separate
`--describe-json` output is an executable-bound description and is not a frame.

The optional CMake build setting `CURVE_FX_DENSE_ARB_SIZING=ON` changes the
arbitrage search from 24 to 96 evaluations and uses a denser size ladder. It
applies to offered and cached-report searches. This research sensitivity mode
is not an `open_session` option and is not currently exposed in the identity
record; retain the CMake setting and executable hash with experiment evidence.
Default builds keep the 24-evaluation search. Results from the two search modes
are not expected to be bit-identical.

Experimental policies may expose cumulative `policy_target_calls`,
`policy_actuator_holds`, and `policy_gate_rejections`. They are available in
full-summary metrics and full traces; `-1` means the policy has no counters.
The counters observe committed paths, roll back with hypothetical/reverted
transactions, and never affect policy decisions. A target call partitions into
an actuator hold, an LP profit-gate rejection, or a committed scale move.
These are research diagnostics, not state writes required by an onchain view.

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
  "scenario_id": "ethusd-2025",
  "price_feed_path": "data/ethusd-reports.npz",
  "market_path": "data/ethusd-1m-candles.json",
  "event_mode": "candles",
  "candle_volume": true,
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

Native arbitrage is a maker. It quotes the pool on the CEX and hedges its fills
on the pool at the next event. The market input describes CEX taker flow in
regular bins. There are two modes:

- **`event_mode="candles"`** (default) approximates the flow from `market_path`,
  a JSON array of six numeric OHLCV fields per row (timestamps in seconds or
  milliseconds).
  - **Clock:** candles lie on one regular clock whose bin is the smallest
    timestamp step. Missing candles are empty bins.
  - **Path:** each candle's taker flow follows its shorter OHLC path (open,
    low, high, close or open, high, low, close; ties go high first). Rising
    legs print taker buys and falling legs taker sells.
  - **Volume:** with `candle_volume=true` (default), the candle volume is spread
    evenly over every 1bp bucket the path crosses. With `false`, the highest buy
    and lowest sell print carry unlimited volume, so fills are limited only by
    the pool.
  - **Input filters:** `candle_filter` clamps wicks and `n_candles` caps input
    rows.
- **`event_mode="trade_flow"`** reads measured flow from `trade_flow_path` (see
  below) and omits `market_path`, `candle_filter`, `n_candles` and
  `candle_volume`.

**Events.** There is one event at each bin end, `t0 + (i+1)*bin_s`, within
`[start_time, end_time]`.
- **Price:** the bin's last trade, carried across empty bins.
- **Volume:** the bin's taker volume.
- **Trace candles:** the bin OHLCV.
- **Exclusions:** bins overlapping an excluded range emit no event and do not
  move the carried price.

**Fill book.** At each event the maker sizes against a fill book built from that
bin's prints:
- **Selling base:** taker buys are the levels it can sell base into, each valued
  at its bucket's lower price edge `exp(b/1e4)`, best first.
- **Buying base:** taker sells are the levels it can buy base from, at
  `exp((b+1)/1e4)`.

Price priority fills a resting quote before any print strictly through it, so a
fill never receives more than that print allowed. The pool trade equals the size
at which a competitive maker ladder, quoting the pool's marginal price, crosses
the volume that printed through it. The arbitrageur keeps the difference between
print and quote, so `arb_pnl_coin0` is an upper bound.

Either side may be empty and the sides may cross. A bin with no print beyond the
pool's fee floor cannot hedge. `pool.costs.arb_fee_bps` is the CEX fee of the
maker's leg. Gas, report costs and report selection apply as described below.

`trade_flow_path` accepts only `.npz` from `data/market/pack_trade_flow.py`:

| Array | Type and shape | Meaning |
| --- | --- | --- |
| `format_version` | uint32 scalar | `1` |
| `bin_s`, `t0` | int64 scalars | bin seconds; first bin start (UTC seconds) |
| `open`, `high`, `low`, `close` | float64 `[N]` | trade prices, NaN in a bin without trades |
| `buy_qty`, `sell_qty` | float64 `[N]` | taker volume by side (base) |
| `ptr` | int64 `[N+1]` | bin `i` owns profile rows `ptr[i]..ptr[i+1]` |
| `side` | uint8 `[M]` | 0 taker buy, 1 taker sell |
| `bucket` | uint32 `[M]` | `floor(1e4*ln(price))` |
| `qty` | float64 `[M]` | positive base volume |

Rows within a bin are strictly ordered by side, then bucket.

The remaining optional session controls are `user_swap_freq_s`,
`user_swap_size_frac`, `user_swap_thresh`, `event_cursor` (`scalar` or
`fast_skip`), `metric_profile`, and `enable_slippage_probes`. Slippage probes
are off unless explicitly enabled.

`excluded_time_ranges` optionally removes UTC Unix-second intervals from a
session, e.g. `[[1760054400, 1760140800]]` excludes 10 October 2025.
- **Format:** pairs are ordered, disjoint and half-open `[start, end)`. Source
  files are unchanged.
- **What is omitted:** events and price-feed samples inside a gap, so excluded
  prints or feeds cannot leak into the next day.
- **Execution:** no actors execute inside a gap. Pool/YB state is retained,
  calendar time and annualization are unchanged, and execution resumes at the
  next retained event; the net price jump across the gap remains.
- **Rejection:** an empty remaining event range is rejected.
- **Replay:** the optimizer records this setting in `run.json` and forwards it
  to exact replay.

`user_swap_size_frac` is the daily fair-TVL utilization fraction. Each scheduled
order has coin0-equivalent notional `fair_tvl * user_swap_size_frac *
user_swap_freq_s / 86400`, converted into the alternating input coin at the
current event price. Thus `1.0` means 100% attempted fair-TVL turnover per day,
not 100% of one reserve per swap.

`yb_mode` is `off`, `active_2l`, or `reference_2l`.
- The enabled modes use `yb_releverage_fee` and `yb_cash_multiplier`, evaluate
  after native arbitrage at every event, and hedge at the event price.
- Native CEX fees and gas are not charged to either YB actor.
- `yb_arb` selects how `active_2l` rebalancing reaches the LEVAMM.
  - `levamm` (default) uses only the fee-paying LEVAMM exchange.
  - `lt_round_trip` first tries the live searchers' LT `deposit` +
    `emergency_withdraw` in one transaction. The deposit adds balanced pool
    liquidity whose cash leg is borrowed from LEVAMM idle cash; the add runs
    the pool's price update and pays its noise/spam fee. The withdrawal
    returns the new shares' pro-rata collateral and debt.
  - LT shares follow the LEVAMM value x0, which is homogeneous of degree one,
    so the LEVAMM moves along its own curve at the post-add oracle, toward the
    pool's cash-per-LP ratio, without the exchange fee.
  - The depositor hedges net coin1 at the external bid/ask and needs profit
    above `yb_round_trip_cost_coin0` (default 6: gas plus the searcher's
    retained floor) plus 1 coin0.
  - When no round trip pays, the fee-paying exchange may still act.
  - `yb_round_trips` counts committed round trips; `yb_releverage_trades`
    counts all fills.
- Summary valuation is hourly for GM accounting and once at the final endpoint
  for raw APY.

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

`scenario_id` and `template_path` are required. `candles` requires
`market_path`; `trade_flow` requires `trade_flow_path`. `price_feed_path` is
optional. The response contains one `scenario` object with event and candle
counts and the first and last event times.

### `evaluate_batch`

`metrics_format` is `object` or `array`, and defaults to `object`.
`metric_fields` optionally selects a non-empty, unique, order-significant subset
of the canonical fields advertised by `hello`; array format requires it.
Candidates carry a unique `ordinal`, a unique `candidate_id`, finite binary64
`policy_params`, and optional `pool_overrides`. Results are sorted by ordinal.

For swap-report policies, `pool_overrides.run.arb_report_rate` is a finite
probability in `[0,1]`, default `1`. Its grid axis is `pool.run.arb_report_rate`.
One fixed SplitMix64 draw keyed by original event ordinal selects report/no-report
before arb sizing; all probes and execution share that choice. Draws are shared
across candidates and independent of batch order, workers, and prior trades.
This is a per-opportunity probability, not a target fraction of executed swaps.
Reports use the attached causal price-feed sample when supplied, otherwise the
current event price. They are
cleared after the arb attempt and cannot be reused by YB, user swaps, or idle ticks.
An attached stale sample remains stale; no current timestamp is fabricated.
Policies without `USES_SWAP_REPORTS` retain their existing feed behavior.

The `aged_fair_fee_dual_ema` policy additionally retains the price and original
timestamp of the last committed report-assisted swap. Later native/YB/user swaps
can use this cache; clearing the per-attempt report context does not erase it.
Its fee is `q + (fallback - q) * min(observation_age / inflation_seconds, 1)`,
where `q` is the base-plus-excess-capture fee capped at fallback. The native actor
compares a supplied report against withholding it by fully sized net profit;
only the selected executed branch can update report memory. Repeated or older
publications cannot replace the cached reference. Newly supplied reports still
pass the admission-age check; cached reports expire by the inflation horizon.
Reports may come from an independent `price_feed_path`: CSV, or numeric NPZ
with exactly `ts` (int64/float64 Unix seconds) and `price` (float64). The NPZ
reader preserves observation timestamps and requires positive prices and strictly
increasing timestamps. It shares the portable MiniZip/NPY decoder with trade-flow tapes.

`pool.run.arb_report_max_age_s = 0` selects the latest independent report at or
before the event, retaining its actual age even across feed gaps. If no independent
feed is configured, legacy same-event CEX reporting remains unchanged.

A positive `arb_report_max_age_s = W` searches all reports in `[event_time-W,
event_time]`, including reports between actor events. Each report is evaluated by
fully sizing the trade against the same pool and remaining finite CEX book.
Selection maximizes executable net arbitrage profit, not the reported price or a
fixed-size fee. Withholding the report is also considered. Only the winning action
commits report memory and liquidity; policy admission rules still apply.

`pool.run.arb_report_count = N` selects the last N observations at or before
an event, including observations between actor events. N must be a whole number
in [0, 1024]; zero preserves the existing time-window mode. A positive count and
a positive `arb_report_max_age_s` are mutually exclusive. Count mode requires an
independent report tape and a swap-report policy. Fewer than N available past
observations uses all of them; future observations are never considered. Across
gaps, report count is not elapsed time: policies charge the original observation
age. Each candidate report and withholding are fully sized from the same state.
Cached-report policies require strictly newer timestamps to replace their cache.

`pool.run.arb_report_random_count = N` models an arb that can obtain just one
report from the latest N observations at or before each event. N is a whole
number in [0, 6]; zero disables this mode. The sampled observation is uniform
over the available last `min(N, available)` reports and is keyed by the original
event ordinal with a SplitMix64 stream independent of the report-offer draw.
Thus the choice is reproducible across candidates, workers, batch order, and
`scalar`/`fast_skip` cursors, while it can differ from event to event. Its true
publication timestamp determines the fee age. The arb fully sizes only the
sampled fresh-report action against withholding/its cached report; it does not
search the other N-1 fresh reports. Random count requires the independent report
tape and swap-report policy, and is mutually exclusive with positive
`arb_report_count`, positive `arb_report_max_age_s`, or a nonnegative offset.
The older `arb_report_count` best-of-N mode remains available for historical
experiment reproduction; new limited-availability scans use random count.

`pool.run.arb_report_offset = K` offers exactly one observation: `0` is the
latest report at or before the event, `1` is one report earlier, and so on.
Its default `-1` preserves count/window selection. A nonnegative offset is
mutually exclusive with positive count or time window. If fewer than `K+1`
past reports exist, no new report is offered. The arb still compares withholding
and its cached reference. The selected report's actual observation timestamp
sets its age, even when reports are separated by gaps.
Only the winning committed swap writes the reference; idle events do not refresh
it. Ties retain the withholding action when it was considered first.

The `sqrt_fair_fee_dual_ema` policy uses ten parameters: base fee, capture,
fallback fee, whole-second expiry, then the unchanged six dual-EMA fields.
Its standalone fee component computes `q = min(F, base + capture*max(edge-base,0))`
with the standard noise floor, then `q + (F-q)*sqrt(min(age/expiry,1))`. No usable
reference, future-dated reference, or age at/above expiry returns fallback.
The same effective submitted-or-cached reference and aging formula govern its
context sizing floor. New submissions do not reset the observation timestamp.

`event_cursor="fast_skip"` is an opt-in `full_summary` optimization for YB off
or `active_2l`. It bypasses an event only when neither the fill book's best
levels clear the native floor gate nor the active YB fee-band gate accepts at
the bin close. The cursor does not extrapolate quiet prices across a block.
`reference_2l` requires the scalar cursor.

The YB gate uses the same marginal LP quote as full sizing. It rejects only when
neither fee-adjusted direction crosses the external LP bid/ask, ignoring
nonnegative fixed and add-liquidity costs (a conservative relaxation). Actual
execution always retains full sizing, costs, and state rollback.

The first observation at or after each mandatory deadline is unskippable:
`last_pool_touch + dustswap_freq_s`, periodic donation/user schedules where
enabled, existing hourly/daily metric samples, and the final observation. A trade
at the dust deadline still takes precedence over an idle tick, exactly as in
scalar mode. Skipping does not advance debt state; projected debt and subsequent
interest/donation settlement continue to use the full actual elapsed time.

Fast-mode detailed traces contain fewer inactive observations. Use scalar for
full chronological replay. The cursor preserves current summary sampling
deadlines rather than inventing approximate interest or donation updates.

Detailed trace rows also expose optional `policy_base_fee`,
`policy_fallback_fee`, and `policy_fee_signal`. Policies may implement the
read-only `fee_diagnostics(state, params, timestamp)` hook to populate them.
Fees are fractions; signal units are policy-specific. A value of `-1` for the
base/fallback means diagnostics are unavailable. Summary grids never evaluate
this hook. These projected controller parameters differ from the existing
`fee` field, which is a small corrective-swap quote.

Native arbitrage applies a conservative fee-floor gate before report selection.
It tests the fill book's best bid and ask. If neither direction can cover the
global fee floor, only native quote sizing is bypassed; YB decisions,
accounting, metrics, and due idle ticks still process that event.

`arb_report_rate` controls whether submitting a report is an available action.
`W` is separate from the policy's fee-inflation lifetime. Historical search is a
myopic best response, not a multi-transaction adversarial strategy solver.


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


### YB rolling geometric-mean APY horizons

`yb_gm30`, `yb_gm60`, and `yb_gm90` summarize 30-, 60-, and 90-day
rolling annualized YB growth returns, sampled hourly using the same YB valuation.
Each is the geometric mean of annualized APYs across all eligible windows in
the evaluated history, not merely the last window. Non-positive or invalid
annualized returns are floored at `1e-20` before taking logs. A horizon with no
eligible windows, or YB disabled, returns `-1`. Samples use the latest stored
observation at or before the window boundary and annualize by actual elapsed time.

`yb_apy_gm` remains an exact alias of `yb_gm90`; existing `yb_gm_windows`,
`yb_gm_floored_windows`, and `yb_gm_floor_share` remain 90-day diagnostics.
The two additional bounded queues advance only at the existing hourly sample
times, with amortized constant work per sample and no additional YB valuation.
These fields require `full_summary`, as do the other YB metrics. Existing stored
results do not acquire new horizons without reevaluation.

### GM floor and report-selection diagnostics

`yb_gm30_floor_share` and `yb_gm60_floor_share` count the fraction of eligible
hourly windows floored before geometric averaging. `yb_gm30_windows` is the
eligible count. `yb_gm30_unfloored` is the geometric mean among windows not
floored, or -1 when none exist; it is diagnostic and does not replace GM30.
`arb_offered_report_trades` / `arb_withheld_report_trades` count successful
arbitrage swaps in report-capable policies whose winning context respectively
contains an offered report / withholds it. These describe the selected action,
not an independent count of newly accepted oracle observations. Their sum is
`trades` for report-capable policies; both are zero for non-report policies.
The diagnostics add no valuations or simulation passes.

`yb_price_scale_hourly_qv` is the annualized sum of squared log price-scale
changes on the same settled hourly sampling clock used by the YB GM metrics:
`sum(log(scale[i]/scale[i-1])^2) * 365 days / sampled_elapsed_seconds`.
It is variance per year (not volatility); take its square root for annualized
volatility. Returns -1 when YB is off or fewer than two distinct-time samples
exist. It follows the run's event availability and truncation, excludes the
partial final hour, and adds one log calculation per hourly YB sample. It is a
diagnostic, not a yield adjustment or a substitute for GM30.

### Post-trade pressure research diagnostics (2026-09-25)

`pool.costs.report_coin0` is a finite, nonnegative candidate-level fixed cost in
coin0, default0. It is added to arbitrage sizing's gas cost only for the alternative
that submits a report. Withholding/cached execution does not pay it. The extra
cost participates in the profit comparison and trade decision; it is not a pool
fee or a balance mutation. It can be scanned through the ordinary candidate/grid
schema. Fast-skip bounds may ignore this positive cost conservatively.

New YB summary metrics are `yb_external_equity_eth`, `yb_external_growth_eth` and
`yb_external_max_drawdown_hourly`. They value LP collateral at external NAV,
subtract projected debt, include pending LT cash, and divide by the external
coin0/coin1 price. Names target the ETHUSD research scenario (coin1=ETH). Growth
starts at the initial pre-trade mark; drawdown samples the settled hourly clock
plus endpoint, not every event. Negative equity is retained. The existing
`yb_gm30`/60/90 metrics and their valuation are unchanged. Missing/disabled YB
uses−1. Stopped histories do not represent full-period endpoint returns.

For policies exposing post-trade pressure state, detailed traces also include
`policy_pressure_base`, `policy_pressure_fallback` (projected fee-unit pressure),
`policy_pressure_fresh_bumps` and `policy_pressure_cached_bumps` (cumulative
certified-swap counts). Unsupported policies use−1. Zero-gain controls can count
certified swaps without changing pressure. These trace observations never mutate
state and are not evaluated in summary grids. `policy_fee_signal` remains the
policy-defined diagnostic (base pressure for the pressure policy).
