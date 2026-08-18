# curve-fx-arb-harness

`curve-fx-arb-harness` owns one transport-free C++17 economic simulation loop and the `curve_fx_eval_v1` evaluator protocol. It consumes an installed `twocrypto::pool`; it does not embed a pool implementation or own grids, optimization, scoring, cluster execution, or plotting. The `curve-fx-optimization` repository invokes this evaluator.

## Build prerequisites and independent setup

Requirements: Python 3.12 with uv, CMake 3.14+, a C++17 compiler, Boost with the JSON component, and Threads. Build/install `twocrypto-cpp` first and point CMake at its install prefix; a sibling source checkout is only an explicit development prerequisite, never a runtime path.

From the pool repository:

```sh
cd /path/to/twocrypto-cpp
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
cmake --install build --prefix "$PWD/_install"
```

Native harness build (no compiled policy):

```sh
cd /path/to/curve-fx-arb-harness
cmake -S . -B build/native -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/twocrypto-cpp/_install
cmake --build build/native --target arb_evaluator_ld --parallel
```

This produces `build/native/arb_evaluator_ld`, the production long-double evaluator. `curve_fx_evaluator` is an internal static build target, not an installed library or stable C++ ABI; the executable protocol is the supported integration boundary.

## Compiled-policy build and digest attestation

The checked-in CHF/USD profile uses this explicit sibling prerequisite. The header must be a regular file and its exact bytes must be attested with `POLICY_EXPECTED_SHA256`; configuration fails on a missing digest or mismatch:

```sh
POLICY=/path/to/curve-fx-optimization/policies/native_policy_dual_ema_stale_cap_v1.hpp
DIGEST=$(shasum -a 256 "$POLICY" | cut -d ' ' -f 1)
cd /path/to/curve-fx-arb-harness
cmake -S . -B build/compiled -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/twocrypto-cpp/_install \
  -DPOLICY_ID=native_policy_dual_ema_stale_cap_v1 \
  -DPOLICY_ABI=twocrypto_policy_v1 \
  -DPOLICY_HEADER_PATH="$POLICY" \
  -DPOLICY_EXPECTED_SHA256="$DIGEST"
cmake --build build/compiled --target arb_evaluator_ld --parallel
```

Record the policy ID, ABI, digest, pool revision, harness revision, compiler/build mode, and binary path in the orchestrator run manifest. The resulting binary identity also contains a binary SHA-256 and policy-source SHA-256; inspect it before use:

```sh
/path/to/curve-fx-arb-harness/build/compiled/arb_evaluator_ld --identity-json
```

## Python client package

The lightweight typed client is a separate package under `python/`:

```sh
cd /path/to/curve-fx-arb-harness/python
uv sync --frozen --extra dev
uv run --frozen --no-sync python -c \
  'import curve_fx_harness_client; print(curve_fx_harness_client.__name__)'
```

It provides one synchronous evaluator subprocess client, strict typed protocol models, and SHA-256/session-attestation helpers. The evaluator's scoring remains in Python orchestrator code; the binary returns raw metrics only.

## Evaluator modes

Identity emits one `hello` frame and exits:

```sh
/path/to/curve-fx-arb-harness/build/native/arb_evaluator_ld --identity-json
```

For a one-candidate smoke, start a short-lived `serve` process, open one
attested session, evaluate once, and shut it down through the same protocol as
a persistent run. There is no divergent one-shot request path.

`serve` is the production persistent NDJSON mode. Stdout is protocol-only and stderr is for logs:

```sh
/path/to/curve-fx-arb-harness/build/native/arb_evaluator_ld serve --workers 1
```

The evaluator defaults to one worker per process so an orchestrator can allocate processes without hidden thread oversubscription. Pass `--workers N` to assign more workers to a process; `N` must not exceed detected hardware concurrency and the effective count is logged to stderr at startup. The v1 `hello` frame remains unchanged for strict existing clients. The orchestrator starts `serve`, consumes `curve_fx_eval_v1` `hello`, opens one immutable attested session, submits `evaluate_batch` frames, and closes the session/shuts down the process. It uses one admitted batch at a time and canonical ordinal ordering.

## Protocol and artifact contract

Every frame is one UTF-8 JSON object terminated by a newline. The lifecycle is `hello` -> `open_session`/`session_ready` -> one or more `evaluate_batch`/`batch_result` exchanges -> `close_session` and `shutdown`. One manifest admits exactly one `resolved_spec.scenario`; the response retains a one-element `scenarios` array for protocol stability. Session paths and expected SHA-256 digests are checked before scenario/template data is loaded. `pool_index` is included in the session config hash and fingerprint. `metric_projection` (`summary` or `full`) controls returned raw metric detail; `observation.kind` (`summary` or `full_trace`) controls trace capture and is independent of economic identity.

Full observation requires a relative artifact directory and writes atomic, run-contained sidecars whose names include the economic fingerprint and content digest. Returned artifact paths and SHA-256 digests are attested by the response. Absolute paths, `..` traversal, and symlink escapes outside the evaluator working directory are rejected. The evaluator returns raw metrics and economic fingerprints; objective scoring and eligibility gates remain in the orchestrator.

See [`protocol/protocol_spec.md`](protocol/protocol_spec.md) for frame schemas and limits.

## Ownership, provenance, and private data

The harness owns feed parsers, `EventSoA`, arbitrage/user flow, donations, the optional YieldBasis 2L model, keepers, metrics, summary/full traces, compiled-policy identity, and the evaluator executable. `yb_mode` selects `off`, metrics-only `passive`, or state-mutating `active_2l`; legacy `yb_releverage=true` maps to `active_2l`. `yb_releverage_fee` and `yb_cash_multiplier` configure enabled modes. The pool owns the installed pool SDK; the orchestrator owns input manifests, data verification, scoring, run artifacts, execution backends, replay, and plots.

Record pool/harness/policy revisions and hashes, compiler/build/numeric mode, scenario/template/input hashes, candidate request, economic configuration, `MetricProjection`, and protocol version for every evaluation. Production data may be Git-LFS material or private fixtures; acquire it through the repository's authorized channel, run orchestrator data verification, and do not assume redistribution or license rights. Do not copy historical binaries, generated runs, or obsolete checkout paths into a build.
