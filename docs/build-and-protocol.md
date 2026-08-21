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
Both targets implement the same v1 protocol and finite binary64 wire boundary.
`arb_evaluator_f64` uses `double` arithmetic; `arb_evaluator_ld` uses the
platform's `long double` arithmetic and reports its actual precision. With no
external header, both instantiate the attested zero-parameter
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

For the checked-in CHF/USD compiled policy, calculate the digest before configuring and pass all four identity inputs:

```sh
POLICY=/path/to/curve-fx-optimization/policies/native_policy_dual_ema_stale_cap_v1.hpp
POLICY_SHA=$(shasum -a 256 "$POLICY" | cut -d ' ' -f 1)
cmake -S /path/to/curve-fx-arb-harness -B /path/to/curve-fx-arb-harness/build/compiled \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/twocrypto-cpp/_install \
  -DPOLICY_ID=native_policy_dual_ema_stale_cap_v1 \
  -DPOLICY_ABI=twocrypto_policy_v1 \
  -DPOLICY_HEADER_PATH="$POLICY" \
  -DPOLICY_EXPECTED_SHA256="$POLICY_SHA"
cmake --build /path/to/curve-fx-arb-harness/build/compiled \
  --target arb_evaluator_f64 arb_evaluator_ld --parallel
```

`--identity-json` reports the strict v1 `hello` frame. `--describe-json`
separately reports the one executable-bound artifact description: source/build
provenance, policy descriptor, and exact v1 lowering paths for every supported
policy, pool, session, and observation parameter. Keeping this description
separate preserves strict old-client parsing of `hello`. Save both records
before submitting an orchestrator run. Evaluators default to one worker per
process; use `--workers N` to allocate more without exceeding detected hardware
concurrency.

## 2. Protocol lifecycle

The production executable is line-oriented:

1. Launch `arb_evaluator_ld serve --workers N`; consume the startup `hello` frame.
2. Send `open_session` with evaluator-visible template/manifest paths and each expected 64-hex SHA-256. The sole manifest envelope is `fxsim_manifest_v1`/`session` with one `resolved_spec.scenario`; each `market` or `chainlink` file carries its SHA-256. The evaluator rejects unknown fields, validates all hashes, and loads immutable scenario data once; wait for `session_ready`.
3. Send `evaluate_batch` with canonical candidate ordinals, policy parameters, pool overrides, a required `metric_projection`, and an observation specification.
4. Consume `batch_result`; results are ordered by ordinal. Because the admitted manifest contains one scenario, `summary` returns its raw metrics as the aggregate and `full` adds its single scenario row. `full_trace` observation additionally returns atomic trace/action/manifest sidecars under the run-contained artifact directory.
5. Send `close_session`, then `shutdown`.

Stdout is reserved for one JSON object per line. Send logs and diagnostics to stderr.

A full-trace sidecar response carries attested paths and SHA-256 values. The evaluator enforces path containment, hash matching, exact compiled-policy parameter count, unique candidate IDs/ordinals, and at most one admitted batch at a time. The effective `pool_index` participates in the session config hash and fingerprint. Every real-valued request input materializes once as a finite IEEE-754 binary64 value. Long-double builds widen that value for arithmetic; they do not recover decimal precision beyond the wire boundary. Candidate parameters and pool overrides are canonicalized from the materialized binary64 value, so equivalent spellings have one identity and adjacent binary64 values remain distinct. Projection and observation level do not change the economic fingerprint.

## 3. Short-lived smoke mode

Use one `serve` lifecycle for a single-candidate smoke. Grid, optimization, and
shiftclick therefore retain the same candidate identity, session attestation,
scoring, and artifact contract whether a process handles one batch or many.

## 4. Python client boundary

The `python/` project is the typed protocol client, not the simulation engine:

```sh
cd /path/to/curve-fx-arb-harness/python
uv sync --frozen --extra dev
uv run --frozen --no-sync python -c \
  'from curve_fx_harness_client import EvaluatorClient; print(EvaluatorClient)'
```

The client starts the binary, checks `hello`, computes/validates input hashes, sends frames, and exposes typed results. The orchestrator remains the only user-facing CLI and owns scores, gates, candidate ranking, and replay normalization.

## 5. Audit bundle

For each binary used by a run, retain the identity frame and
`curve_fx_evaluator_description_v1` record and record:

- harness and installed pool revisions;
- compiler ID/version, CMake build type, numeric mode, target;
- policy ID/ABI/header SHA-256 (`none` for the native pool policy);
- binary SHA-256;
- template, scenario manifest, and market-feed SHA-256 values; and
- protocol version, metric projection, and economic configuration.

Do not treat a binary path alone as provenance. Keep outputs below the orchestrator run directory and do not use an obsolete checkout or source-relative path as a runtime dependency.
