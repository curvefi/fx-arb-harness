# Harness build and evaluator protocol

## 1. Build a reproducible binary

Install the pool target first, then configure this repository with `CMAKE_PREFIX_PATH` pointing at that install tree. Native and compiled-policy builds must use separate build directories:

```sh
cmake -S /path/to/curve-fx-arb-harness -B /path/to/curve-fx-arb-harness/build/native \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/twocrypto-cpp/_install
cmake --build /path/to/curve-fx-arb-harness/build/native \
  --target arb_evaluator_f64 arb_evaluator_ld --parallel
```
Both targets implement the same `curve_fx_eval` protocol and finite binary64 wire boundary.
`arb_evaluator_f64` uses `double` arithmetic; `arb_evaluator_ld` uses the
platform's `long double` arithmetic and reports its actual precision. With no
external header, both instantiate the zero-parameter
`native_passthrough` compiled policy rather than a separate runtime-native path.
The throughput flags `CURVE_FX_ENABLE_IPO` and `CURVE_FX_NATIVE_TUNING` are opt-in. Both default to `OFF`, which keeps binaries suitable for heterogeneous blades. For a Release binary dedicated to one compatible host, enable them explicitly:

```sh
cmake -S /path/to/curve-fx-arb-harness -B /path/to/curve-fx-arb-harness/build/tuned \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/twocrypto-cpp/_install \
  -DCURVE_FX_ENABLE_IPO=ON \
  -DCURVE_FX_NATIVE_TUNING=ON
cmake --build /path/to/curve-fx-arb-harness/build/tuned \
  --target arb_evaluator_ld --parallel
```

`CURVE_FX_ENABLE_IPO` requests compiler-supported interprocedural optimization and fails configuration when unsupported. `CURVE_FX_NATIVE_TUNING` adds host-specific tuning and is not portable across blades; keep it `OFF` for shared or heterogeneous deployment. The selected values are included in evaluator identity.

GCC builds also support explicit two-pass PGO. Configure and build the same
build directory with `-DCURVE_FX_PGO=generate` and an absolute
`-DCURVE_FX_PGO_DIR`, exercise the instrumented evaluator on a representative
corpus using one evaluator worker, then reconfigure that directory with
`-DCURVE_FX_PGO=use` and rebuild.
The final identity reports `pgo_mode=use`; instrumented `generate` binaries are
training artifacts, not production evaluators.

For the checked-in CHF/USD compiled policy, pass the policy identity inputs:

```sh
POLICY=/path/to/curve-fx-optimization/policies/native_policy_dual_ema_stale_cap_v1.hpp
cmake -S /path/to/curve-fx-arb-harness -B /path/to/curve-fx-arb-harness/build/compiled \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/twocrypto-cpp/_install \
  -DPOLICY_ID=native_policy_dual_ema_stale_cap_v1 \
  -DPOLICY_ABI=twocrypto_policy_v1 \
  -DPOLICY_HEADER_PATH="$POLICY"
cmake --build /path/to/curve-fx-arb-harness/build/compiled \
  --target arb_evaluator_f64 arb_evaluator_ld --parallel
```

`--identity-json` reports the strict `curve_fx_eval` `hello` frame. `--describe-json`
separately reports the one executable-bound artifact description: source/build
source/build identity, policy descriptor, and exact lowering paths for every supported
policy, pool, session, and observation parameter. Keeping this description
separate keeps `hello` small and strict. Save both records
before submitting an orchestrator run. Evaluators default to one worker per
process; use `--workers N` to allocate more without exceeding detected hardware
concurrency.

## 2. Protocol lifecycle

The production executable is line-oriented:

1. Launch `arb_evaluator_ld serve --workers N`; consume the startup `hello` frame.
2. Send `open_session` with evaluator-visible `template_path`, `scenario_id`, `market_path`, and optional `chainlink_path`. `event_cursor` is `scalar` by default; `exact_skip` is a guarded optimization that automatically falls back when exact skipping is unsupported. `metric_profile` defaults to `full_summary`; `grid_core` is the exact no-YB summary profile and admits only its fixed production field set. The evaluator rejects unknown fields and loads immutable scenario data once; wait for `session_ready`.
3. Send `evaluate_batch` with either canonical candidate objects or one compact
   Cartesian `grid` plus a non-empty array of global `ordinals`. The latter is
   advertised by the `grid_ordinals` capability and lets exhaustive-grid clients
   avoid materializing candidates in Python. Both forms require a
   `metric_projection` and observation specification and cannot be mixed.
4. Consume `batch_result`; results are ordered by ordinal. Because each session contains one scenario, `summary` returns its raw metrics and `full` adds its scenario row. `full_trace` additionally returns `trace_path` and optional `actions_path`.
5. Send `close_session`, then `shutdown`.

Stdout is reserved for one JSON object per line. Send logs and diagnostics to stderr.

A full-trace sidecar response carries paths only. The evaluator enforces exact compiled-policy parameter count, unique candidate IDs/ordinals, and at most one admitted batch at a time. Every real-valued request input materializes once as a finite IEEE-754 binary64 value. Long-double builds widen that value for arithmetic; they do not recover decimal precision beyond the wire boundary.

## 3. One-batch smoke

Use the same `serve` lifecycle for a single-candidate smoke as for repeated
batches. The evaluator has no separate one-shot execution path.

## 4. Python client boundary

The `python/` project is the typed protocol client, not the simulation engine:

```sh
cd /path/to/curve-fx-arb-harness/python
uv sync --frozen --extra dev
uv run --frozen --no-sync python -c \
  'from curve_fx_harness_client import EvaluatorClient; print(EvaluatorClient)'
```

The client starts the binary, checks `hello`, sends path-based frames, and exposes typed results. The orchestrator remains the only user-facing CLI and owns scores, gates, candidate ranking, and replay normalization.

## 5. Optimizer records

For each binary used by a run, retain the identity frame and
`curve_fx_evaluator_description_v1` record in the optimizer's run record and record:

- harness and installed pool revisions;
- compiler ID/version, CMake build type, numeric mode, target;
- policy ID/ABI/header path (`none` for the native pool policy);
- evaluator executable path;
- template, scenario, and market-feed paths; and
- protocol name, metric projection, and economic configuration.

The optimizer owns TOML/run.json/results.npz, scoring, plotting, replay, and placement.
