# curve-fx-arb-harness

`curve-fx-arb-harness` owns one transport-free C++17 economic simulation loop and the `curve_fx_eval` evaluator protocol. It consumes an installed `twocrypto::pool`; it does not embed a pool implementation or own client-side scoring or artifact presentation.

## Repository split

- [`twocrypto-cpp`](https://github.com/curvefi/twocrypto-cpp) — C++ Twocrypto pool implementation and Vyper parity; no market simulation or experiment orchestration.
- [`fx-arb-harness`](https://github.com/curvefi/fx-arb-harness) — C++ arbitrage simulation and evaluator protocol; owns market-event execution and raw metrics.
- [`fx-optimization`](https://github.com/curvefi/fx-optimization) — cluster orchestration, parameter grids, scoring, result storage, robustness analysis, heatmaps, and replay.

## Build prerequisites and independent setup

Requirements: Python 3.12 with uv, CMake 3.14+, a C++17 compiler, Boost with the JSON component, and Threads. Build/install `twocrypto-cpp` first and point CMake at its install prefix; a sibling source checkout is only an explicit development prerequisite, never a runtime path.

From the pool repository:

```sh
cd /path/to/twocrypto-cpp
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
cmake --install build --prefix "$PWD/_install"
```

Default compiled-passthrough build (no external policy header):

```sh
cd /path/to/curve-fx-arb-harness
cmake -S . -B build/native -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/twocrypto-cpp/_install
cmake --build build/native \
  --target arb_evaluator_f64 arb_evaluator_ld --parallel
```

This produces the binary64-arithmetic `arb_evaluator_f64` and the
`long double`-arithmetic `arb_evaluator_ld`. Both use the compiled
`native_passthrough` policy with `policy_params=[]`; its zero hooks delegate fee
and price-scale decisions to native pool mechanics. The typed evaluator
libraries are internal build targets, not stable C++ ABIs; the executable
protocol is the supported integration boundary.

## Compiled-policy build

Use an external policy header supplied as a regular file:

```sh
POLICY=/path/to/policy.hpp
cd /path/to/curve-fx-arb-harness
cmake -S . -B build/compiled -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/twocrypto-cpp/_install \
  -DPOLICY_ID=your_policy_id \
  -DPOLICY_ABI=twocrypto_policy_v1 \
  -DPOLICY_HEADER_PATH="$POLICY"
cmake --build build/compiled \
  --target arb_evaluator_f64 arb_evaluator_ld --parallel
```

Record the policy ID, ABI, pool revision, harness revision, compiler/build mode, and binary path in the optimizer run record. Inspect both the protocol identity and the separate executable-bound build record before use:

```sh
/path/to/curve-fx-arb-harness/build/compiled/arb_evaluator_ld --identity-json
/path/to/curve-fx-arb-harness/build/compiled/arb_evaluator_ld --describe-json
```

## Python client package

The lightweight typed client is a separate package under `python/`:

```sh
cd /path/to/curve-fx-arb-harness/python
uv sync --frozen --extra dev
uv run --frozen --no-sync python -c \
  'import curve_fx_harness_client; print(curve_fx_harness_client.__name__)'
```

It provides one synchronous evaluator subprocess client and strict typed protocol models. The evaluator's scoring remains in Python orchestrator code; the binary returns raw metrics only.

## Evaluator modes

Identity emits one `hello` frame and exits:

```sh
/path/to/curve-fx-arb-harness/build/native/arb_evaluator_ld --identity-json
```

`--describe-json` is not a protocol frame. It is the canonical inspectable
artifact description: source/build identity plus the compiled policy descriptor
and exact lowering paths for policy, pool, session, and observation parameters.

For a one-candidate smoke, start a short-lived `serve` process, open one
session, evaluate once, and shut it down through the same protocol as
a persistent run. There is no divergent one-shot request path.

`serve` is the production persistent NDJSON mode. Stdout is protocol-only and stderr is for logs:

```sh
/path/to/curve-fx-arb-harness/build/native/arb_evaluator_ld serve --workers 1
```

The evaluator defaults to one worker per process so an orchestrator can allocate processes without hidden thread oversubscription. Pass `--workers N` to assign more workers to a process; `N` must not exceed detected hardware concurrency and the effective count is logged to stderr at startup. The orchestrator starts `serve`, consumes a `curve_fx_eval` `hello`, opens one immutable session, submits `evaluate_batch` frames, and closes the session/shuts down the process. It uses one admitted batch at a time and canonical ordinal ordering.

## Protocol and artifact contract

Every frame is one UTF-8 JSON object terminated by a newline. The lifecycle is `hello` -> `open_session`/`session_ready` -> one or more `evaluate_batch`/`batch_result` exchanges -> `close_session` and `shutdown`. All real-valued request inputs materialize once as finite IEEE-754 binary64 values, then widen to the evaluator arithmetic type when needed. One session admits exactly one scenario. `open_session` supplies `template_path`, `scenario_id`, `market_path`, and optional `chainlink_path`; `yb_mode` is the canonical YB selector. `event_cursor` selects the permanent `scalar` reference or the guarded `exact_skip` cursor, which falls back to scalar whenever its exactness preconditions are absent. `metric_profile` selects `full_summary` or the exact no-YB `grid_core` field set; the latter rejects unsupported fields instead of returning approximations. GridCore reports `apy_net_consistency_90d`, the APY corresponding to the mean minus one population standard deviation of daily-sampled, annualized 90-day net log returns. The full profile retains legacy `apy_net_gm`. `metric_projection` (`summary` or `full`) controls returned raw metric detail; `observation.kind` (`summary` or `full_trace`) controls trace capture.

Full observation writes an atomic trace sidecar and, when requested, an action sidecar. The response returns `trace_path` and optional `actions_path` only. The evaluator returns raw metrics; objective scoring, plotting, replay, and placement remain in the optimizer.

See [`protocol/protocol_spec.md`](protocol/protocol_spec.md) for frame schemas and limits.

## Ownership, inputs, and private data

The harness owns feed parsers, `EventSoA`, arbitrage/user flow, donations, the optional YieldBasis 2L model, keepers, metrics, summary/full traces, compiled-policy identity, and the evaluator executable. `yb_mode` selects `off`, established Observer2-equivalent `active_2l`, or contract-derived candidate `reference_2l`; `yb_releverage_fee` and `yb_cash_multiplier` configure enabled modes. Active and reference evaluate after every causal event. Summary-mode YB valuation runs hourly for GM accounting and once at the final endpoint for raw APY, while detailed replay additionally values logged rows. `reference_2l` executes represented VirtualPool/LevAMM/native route arithmetic but retains synthetic fresh-L2 state and source-free event timing; it is not proven contract parity, a full LT model, or a historical-onchain replay. Each reference decision scans 24 complete routes per direction and admits profit above 1 coin0, so the mode is intended for finalist diagnostics rather than large discovery grids. The pool owns the installed pool SDK; the optimizer owns TOML/run.json/results.npz, scoring, plotting, replay, and placement.

Inputs are ordinary paths supplied by the optimizer TOML. Acquisition of private or Git-LFS data is user-owned; do not assume redistribution or license rights. Do not copy historical binaries, generated runs, or obsolete checkout paths into a build.
