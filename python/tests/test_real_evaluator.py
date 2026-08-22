import json
import os
import subprocess
from pathlib import Path

import pytest

from curve_fx_harness_client import EvaluatorClient
from curve_fx_harness_client.exceptions import RemoteEvaluatorError
from curve_fx_harness_client.models import CandidateSpec, ObservationSpec


EVALUATOR = os.environ.get("CURVE_FX_EVALUATOR")
pytestmark = pytest.mark.skipif(
    not EVALUATOR,
    reason="CURVE_FX_EVALUATOR is required for the real protocol test",
)


def _write_inputs(root: Path) -> tuple[Path, Path]:
    template = root / "template.json"
    template.write_text(
        json.dumps(
            {
                "pools": [
                    {
                        "tag": "protocol_smoke",
                        "pool": {
                            "initial_liquidity": ["100000000000000000000000", "100000000000000000000000"],
                            "A": "500000.0",
                            "gamma": "100000000000000",
                            "mid_fee": "1000000.0",
                            "out_fee": "20000000.0",
                            "fee_gamma": "100000000000000000",
                            "adjustment_step_min": "100000000",
                            "adjustment_step_max": "5000000000000000",
                            "ma_time": "865",
                            "reserved_profit_fraction": "5000000000.0",
                            "admin_fee": "0",
                            "policy": {"kind": "none"},
                            "initial_price": "1000000000000000000",
                            "start_timestamp": "1700000000",
                            "donation_apy": "0",
                            "donation_frequency": "3600",
                            "donation_duration": "604800",
                            "donation_coins_ratio": "0.5",
                        },
                        "costs": {
                            "arb_fee_bps": 10,
                            "gas_coin0": 0,
                            "use_volume_cap": False,
                            "volume_cap_mult": 1,
                        },
                    }
                ]
            },
            sort_keys=True,
        ),
        encoding="utf-8",
    )

    candles = root / "candles.json"
    rows = []
    for index in range(24):
        timestamp = 1_700_000_000 + index * 60
        price = 1.0 + ((index % 5) - 2) * 0.0001
        rows.append([timestamp, price, price + 0.0001, price - 0.0001, price, 1_000_000.0])
    candles.write_text(json.dumps(rows, separators=(",", ":")), encoding="utf-8")

    return template, candles


def test_description_is_bound_and_complete() -> None:
    assert EVALUATOR is not None
    completed = subprocess.run(
        [EVALUATOR, "--describe-json"],
        check=True,
        capture_output=True,
        text=True,
    )
    info = json.loads(completed.stdout)
    identity = json.loads(subprocess.run(
        [EVALUATOR, "--identity-json"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout)["evaluator_identity"]
    assert info["schema_version"] == "curve_fx_evaluator_description_v1"
    assert {"version", "revision", "dirty"} <= info["harness"].keys()
    assert {"version", "revision", "dirty"} <= info["pool"].keys()
    assert info["build"]["type"]
    assert info["build"]["real_digits"] >= info["build"]["wire_real_digits"]
    assert info["build"]["wire_real_type"] == "IEEE-754 binary64"
    assert info["build"]["wire_real_digits"] == 53
    assert info["build"]["target"] == identity["build_target"]
    assert info["build"]["numeric_mode"] == identity["numeric_mode"]
    assert info["build"]["real_type"] == identity["real_type"]
    assert info["policy"]["id"] == identity["policy_id"]
    assert info["policy"]["parameter_count"] == identity["policy_parameter_count"]
    schema_canonical_json = info["parameter_schema_canonical_json"]
    assert json.loads(schema_canonical_json) == info["parameter_schema"]

    parameters = info["parameter_schema"]["parameters"]
    policy_parameters = [p for p in parameters if p["name"].startswith("policy.")]
    assert len(policy_parameters) == info["policy"]["parameter_count"]
    if identity["policy_id"] == "native_passthrough":
        assert policy_parameters == []
    else:
        assert [p["name"] for p in policy_parameters] == [
            "policy.fast_half_life_s",
            "policy.slow_half_life_s",
            "policy.kappa",
            "policy.min_cap_bps",
            "policy.deadband_bps",
        ]
        assert [p["order"] for p in policy_parameters] == list(range(5))
        assert {p["type"] for p in policy_parameters} == {"real"}
        assert {p["wire_representation"] for p in policy_parameters} == {"finite_binary64"}
        assert [
            (p["default"], p["minimum"], p["maximum"], p["quantum"])
            for p in policy_parameters
        ] == [
            (3600.0, 60.0, 86400.0, 10.0),
            (86400.0, 60.0, 604800.0, 10.0),
            (1.0, 0.0, 5.0, 0.05),
            (10.0, 0.0, 250.0, 0.5),
            (0.0, 0.0, 100.0, 0.5),
        ]

    pool_names = {p["name"] for p in parameters if p["name"].startswith("pool.")}
    assert "pool.policy" not in pool_names
    assert pool_names == set("""
        pool.tag pool.initial_liquidity pool.A pool.gamma pool.mid_fee
        pool.out_fee pool.fee_gamma pool.adjustment_step_min
        pool.adjustment_step_max pool.ma_time pool.reserved_profit_fraction
        pool.admin_fee pool.initial_price pool.start_timestamp
        pool.historical_state pool.donation_apy pool.donation_frequency
        pool.donation_duration pool.initial_donation_days
        pool.donation_coins_ratio pool.user_swap_size_frac
        pool.costs.arb_fee_bps pool.costs.gas_coin0
        pool.costs.use_volume_cap pool.costs.volume_cap_mult
        pool.costs.volume_cap_is_coin_1
    """.split())
    run_names = {p["name"] for p in parameters if p["name"].startswith("run.")}
    assert run_names == set("""
        run.session_id run.template_path run.scenario_id run.market_path
        run.chainlink_path run.pool_index run.n_candles run.start_time
        run.end_time run.candle_filter run.min_swap run.max_swap
        run.dustswap_freq_s run.dustswap_random run.dustswap_dynamic_freq_s
        run.dustswap_dynamic_gap_enabled run.dustswap_dynamic_gap_bps
        run.dustswap_dynamic_heartbeat_s run.dustswap_commit_clock_freq_s
        run.policy_keeper_enabled run.allow_hybrid_keeper run.user_swap_freq_s
        run.user_swap_size_frac run.user_swap_thresh
        run.disable_slippage_probes run.yb_mode
        run.yb_releverage_fee run.yb_cash_multiplier run.metric_projection
        run.observation.kind run.observation.trace_interval
        run.observation.trace_actions run.observation.artifact_dir
    """.split())
    assert all({
        "name", "lowering_path", "type", "unit",
        "wire_representation", "classification",
    } <= parameter.keys() for parameter in parameters)
    enums = {p["name"]: p["choices"] for p in parameters if p["type"] == "enum"}
    assert enums == {
        "run.yb_mode": ["off", "passive", "active_2l"],
        "run.metric_projection": ["summary", "full"],
        "run.observation.kind": ["summary", "full_trace"],
    }


def _open(client: EvaluatorClient, template: Path, candles: Path, session_id: str):
    return client.open_session(
        session_id=session_id,
        template_path=template,
        scenario_id="protocol-smoke",
        market_path=candles,
        start_time=1_700_000_000,
        end_time=1_700_000_000 + 9 * 60,
        candle_filter=100.0,
    )


def _economic_metrics(result) -> dict[str, object]:
    return {key: value for key, value in result.metrics.items() if key != "elapsed_ms"}


def test_real_persistent_batches_match_single_candidate_results(tmp_path: Path) -> None:
    assert EVALUATOR is not None
    template, candles = _write_inputs(tmp_path)
    client = EvaluatorClient(EVALUATOR, work_dir=tmp_path)
    try:
        hello = client.start()
        policy_params = json.loads(os.environ.get("CURVE_FX_POLICY_PARAMS", "[]"))
        assert len(policy_params) == hello.evaluator_identity.policy_parameter_count
        _open(client, template, candles, "batch-order")

        single = client.evaluate_batch([
            CandidateSpec(
                ordinal=7,
                candidate_id="single",
                policy_params=policy_params,
            )
        ]).results[0]
        batch = client.evaluate_batch([
            CandidateSpec(
                ordinal=7,
                candidate_id="batched",
                policy_params=policy_params,
            ),
            CandidateSpec(
                ordinal=3,
                candidate_id="other",
                policy_params=policy_params,
            ),
        ])

        assert [result.ordinal for result in batch.results] == [3, 7]
        batched = next(result for result in batch.results if result.ordinal == 7)
        assert _economic_metrics(batched) == _economic_metrics(single)
    finally:
        client.shutdown()


def test_real_evaluator_rejects_missing_market_file(tmp_path: Path) -> None:
    assert EVALUATOR is not None
    template, _ = _write_inputs(tmp_path)

    client = EvaluatorClient(EVALUATOR, work_dir=tmp_path)
    try:
        with pytest.raises(FileNotFoundError, match="Market file not found"):
            _open(client, template, tmp_path / "missing.json", "missing-market")
    finally:
        client.shutdown()


def test_pool_index_selects_another_template_entry(tmp_path: Path) -> None:
    assert EVALUATOR is not None
    template, candles = _write_inputs(tmp_path)
    template_data = json.loads(template.read_text(encoding="utf-8"))
    duplicate = json.loads(json.dumps(template_data["pools"][0]))
    duplicate["tag"] = "protocol_smoke_duplicate"
    template_data["pools"].append(duplicate)
    template.write_text(
        json.dumps(template_data, sort_keys=True), encoding="utf-8"
    )

    ready = []
    for pool_index in (0, 1):
        client = EvaluatorClient(EVALUATOR, work_dir=tmp_path)
        try:
            ready.append(client.open_session(
                session_id=f"pool-{pool_index}",
                template_path=template,
                scenario_id="protocol-smoke",
                market_path=candles,
                start_time=1_700_000_000,
                end_time=1_700_000_000 + 9 * 60,
                candle_filter=100.0,
                pool_index=pool_index,
            ))
        finally:
            client.shutdown()

    assert [item.scenarios[0].id for item in ready] == ["protocol-smoke", "protocol-smoke"]


def test_real_evaluator_rejects_missing_metric_projection(tmp_path: Path) -> None:
    assert EVALUATOR is not None
    template, candles = _write_inputs(tmp_path)
    policy_params = json.loads(os.environ.get("CURVE_FX_POLICY_PARAMS", "[]"))
    process = subprocess.Popen(
        [EVALUATOR, "serve"],
        cwd=tmp_path,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
        bufsize=1,
    )
    assert process.stdin is not None
    assert process.stdout is not None
    try:
        hello = json.loads(process.stdout.readline())
        assert hello["type"] == "hello"
        assert len(policy_params) == hello["evaluator_identity"]["policy_parameter_count"]

        process.stdin.write(json.dumps({
            "protocol": "curve_fx_eval",
            "type": "open_session",
            "request_id": "open-1",
            "session_id": "missing-projection",
            "template_path": str(template),
            "scenario_id": "protocol-smoke",
            "market_path": str(candles),
            "start_time": 1_700_000_000,
            "end_time": 1_700_000_000 + 9 * 60,
            "candle_filter": 100.0,
        }) + "\n")
        process.stdin.flush()
        ready = json.loads(process.stdout.readline())
        assert ready["type"] == "session_ready"

        process.stdin.write(json.dumps({
            "protocol": "curve_fx_eval",
            "type": "evaluate_batch",
            "request_id": "batch-1",
            "session_id": "missing-projection",
            "candidates": [{
                "ordinal": 0,
                "candidate_id": "candidate-0",
                "policy_params": policy_params,
            }],
        }) + "\n")
        process.stdin.flush()
        error = json.loads(process.stdout.readline())
        assert error["type"] == "error"
        assert error["error_code"] == "MISSING_REQUIRED_FIELD"
        assert "metric_projection" in error["message"]

        process.stdin.write(json.dumps({
            "protocol": "curve_fx_eval",
            "type": "evaluate_batch",
            "request_id": "batch-2",
            "session_id": "missing-projection",
            "metric_projection": "summary",
            "candidates": [{
                "ordinal": 0,
                "candidate_id": "candidate-nonfinite",
                "policy_params": policy_params,
                "pool_overrides": {"pool": {"A": "nan"}},
            }],
        }) + "\n")
        process.stdin.flush()
        error = json.loads(process.stdout.readline())
        assert error["type"] == "error"
        assert error["error_code"] == "INVALID_POOL_OVERRIDES"
        assert "finite binary64" in error["message"]

        process.stdin.write(json.dumps({
            "protocol": "curve_fx_eval",
            "type": "shutdown",
            "request_id": "shutdown-1",
        }) + "\n")
        process.stdin.flush()
        assert process.wait(timeout=5) == 0
    finally:
        if process.poll() is None:
            process.terminate()
            process.wait(timeout=5)


def test_real_evaluator_runs_optional_yb_2l_model(tmp_path: Path) -> None:
    assert EVALUATOR is not None
    template, candles = _write_inputs(tmp_path)
    client = EvaluatorClient(EVALUATOR, work_dir=tmp_path)
    try:
        hello = client.start()
        policy_params = json.loads(os.environ.get("CURVE_FX_POLICY_PARAMS", "[]"))
        assert len(policy_params) == hello.evaluator_identity.policy_parameter_count
        client.open_session(
            session_id="yb-2l",
            template_path=template,
            scenario_id="protocol-smoke",
            market_path=candles,
            yb_mode="active_2l",
            yb_releverage_fee=0.013,
            yb_cash_multiplier=3.0,
        )
        result = client.evaluate_batch(
            [CandidateSpec(
                ordinal=0,
                candidate_id="yb-2l",
                policy_params=policy_params,
            )],
            observation=ObservationSpec(
                kind="full_trace",
                trace_interval=1,
                artifact_dir="yb-trace",
            ),
        ).results[0]
        assert result.status == "ok"
        assert result.metrics["yb_enabled"] == 1.0
        assert result.metrics["yb_fee"] == pytest.approx(0.013)
        assert result.artifacts is not None
        trace = json.loads(
            (tmp_path / result.artifacts.trace_path).read_text(encoding="utf-8")
        )
        assert trace
        assert any(row["yb_debt"] > 0 and row["yb_collateral_lp"] > 0 for row in trace)
    finally:
        client.shutdown()


def test_real_evaluator_yb_mode_round_trip(tmp_path: Path) -> None:
    assert EVALUATOR is not None
    template, candles = _write_inputs(tmp_path)
    policy_params = json.loads(os.environ.get("CURVE_FX_POLICY_PARAMS", "[]"))

    def _run(mode: str, *, fee: float = 0.012, cash: float = 1.0):
        client = EvaluatorClient(EVALUATOR, work_dir=tmp_path)
        try:
            hello = client.start()
            assert len(policy_params) == hello.evaluator_identity.policy_parameter_count
            assert hello.yb_modes == ["off", "passive", "active_2l"]
            client.open_session(
                session_id=f"yb-{mode}",
                template_path=template,
                scenario_id="protocol-smoke",
                market_path=candles,
                yb_mode=mode,
                yb_releverage_fee=fee,
                yb_cash_multiplier=cash,
            )
            return client.evaluate_batch(
                [CandidateSpec(ordinal=0, candidate_id=f"yb-{mode}", policy_params=policy_params)]
            ).results[0]
        finally:
            client.shutdown()

    passive = _run("passive", fee=0.011, cash=2.5)
    assert passive.status == "ok"
    assert passive.metrics["yb_enabled"] == 1.0
    assert passive.metrics["yb_fee"] == pytest.approx(0.011)
    assert passive.metrics["yb_apy"] != -1.0
    assert passive.metrics["yb_final_growth"] > 0.0

    off = _run("off")
    assert off.status == "ok"
    assert off.metrics["yb_enabled"] == 0.0
    assert off.metrics["yb_apy"] == -1.0
    assert off.metrics["yb_fee"] == 0.0

    client = EvaluatorClient(EVALUATOR, work_dir=tmp_path)
    try:
        client.start()
        with pytest.raises(ValueError, match="yb_mode"):
            client.open_session(
                session_id="yb-bad",
                template_path=template,
                scenario_id="protocol-smoke",
                market_path=candles,
                yb_mode="strange",
            )
    finally:
        client.shutdown()


def test_real_persistent_short_lived_trace_and_paths(tmp_path: Path) -> None:
    assert EVALUATOR is not None
    template, candles = _write_inputs(tmp_path)

    persistent = EvaluatorClient(EVALUATOR, work_dir=tmp_path)
    try:
        hello = persistent.start()
        policy_params = json.loads(os.environ.get("CURVE_FX_POLICY_PARAMS", "[]"))
        assert len(policy_params) == hello.evaluator_identity.policy_parameter_count
        candidate = CandidateSpec(
            ordinal=0,
            candidate_id="candidate-0",
            policy_params=policy_params,
        )
        ready = _open(persistent, template, candles, "persistent")
        assert ready.scenarios[0].candles_count == 10
        assert ready.scenarios[0].end_ts == 1_700_000_000 + 9 * 60
        summary_frame = persistent.evaluate_batch([candidate], metric_projection="summary")
        summary = summary_frame.results[0]
        if hello.evaluator_identity.policy_id == "native_passthrough":
            assert policy_params == []
            assert summary.status == "ok"
        with pytest.raises(RemoteEvaluatorError, match="pool_overrides.pool.policy"):
            persistent.evaluate_batch([
                CandidateSpec(
                    ordinal=0,
                    candidate_id="runtime-native-kind",
                    policy_params=policy_params,
                    pool_overrides={"pool": {"policy": {"kind": "twocrypto_policy"}}},
                )
            ])
        numeric_override = persistent.evaluate_batch([
            CandidateSpec(
                ordinal=0,
                candidate_id="override-number",
                policy_params=policy_params,
                pool_overrides={"pool": {"A": 500000.0}},
            )
        ]).results[0]
        string_override = persistent.evaluate_batch([
            CandidateSpec(
                ordinal=0,
                candidate_id="override-string",
                policy_params=policy_params,
                pool_overrides={"pool": {"A": "500000.0"}},
            )
        ]).results[0]
        assert _economic_metrics(numeric_override) == _economic_metrics(string_override)
        canonical = persistent.evaluate_batch([
            CandidateSpec(
                ordinal=9,
                candidate_id="candidate-9",
                policy_params=policy_params,
            ),
            CandidateSpec(
                ordinal=3,
                candidate_id="candidate-3",
                policy_params=policy_params,
            ),
        ])
        assert [result.ordinal for result in canonical.results] == [3, 9]
        full_frame = persistent.evaluate_batch(
            [candidate],
            metric_projection="full",
            observation=ObservationSpec(
                kind="full_trace",
                trace_interval=1,
                trace_actions=True,
                artifact_dir="trace",
            ),
        )
        full = full_frame.results[0]
        assert summary_frame.metric_projection.value == "summary"
        assert full_frame.metric_projection.value == "full"
        assert summary.scenario_results == []
        assert len(full.scenario_results) == 1
        assert summary.status == full.status == "ok"
        assert summary.candidate_id == full.candidate_id == "candidate-0"
        assert _economic_metrics(summary) == _economic_metrics(full)
        assert summary.metrics["yb_enabled"] == 0.0
        assert summary.metrics["yb_apy"] == -1.0
        assert full.artifacts is not None

        trace_path = tmp_path / full.artifacts.trace_path
        trace = json.loads(trace_path.read_text(encoding="utf-8"))
        assert trace and {"xp_0", "p_chainlink", "yb_debt"} <= trace[0].keys()
        actions_path = tmp_path / full.artifacts.actions_path
        actions = json.loads(actions_path.read_text(encoding="utf-8"))
        assert all("type" in action for action in actions)
        assert {path.name for path in (tmp_path / "trace").iterdir()} == {
            trace_path.name,
            actions_path.name,
        }

        full_repeat = persistent.evaluate_batch(
            [candidate],
            observation=ObservationSpec(
                kind="full_trace",
                trace_interval=1,
                trace_actions=True,
                artifact_dir="trace",
            ),
        ).results[0]
        assert full_repeat.artifacts == full.artifacts

        wrong_count = CandidateSpec(
            ordinal=1,
            candidate_id="wrong-count",
            policy_params=[*policy_params, 1.0],
        )
        with pytest.raises(RemoteEvaluatorError, match="expected .* policy parameters"):
            persistent.evaluate_batch([wrong_count])

        duplicate = CandidateSpec(
            ordinal=0,
            candidate_id="candidate-0",
            policy_params=policy_params,
        )
        with pytest.raises(RemoteEvaluatorError, match="must both be unique"):
            persistent.evaluate_batch([candidate, duplicate])

        unknown_override = CandidateSpec(
            ordinal=2,
            candidate_id="unknown-override",
            policy_params=policy_params,
            pool_overrides={"pool": {"mid_fee_bps": 10}},
        )
        rejected = persistent.evaluate_batch([unknown_override]).results[0]
        assert rejected.status == "failed"
        assert "unknown field: mid_fee_bps" in (rejected.error or "")
    finally:
        persistent.shutdown()

    short_lived = EvaluatorClient(EVALUATOR, work_dir=tmp_path)
    try:
        ready_once = _open(short_lived, template, candles, "once")
        once = short_lived.evaluate_batch([candidate]).results[0]
        assert _economic_metrics(once) == _economic_metrics(summary)
    finally:
        short_lived.shutdown()

    unsafe_client = EvaluatorClient(EVALUATOR, work_dir=tmp_path)
    try:
        with pytest.raises(RemoteEvaluatorError, match="unsafe characters"):
            unsafe_client.open_session(
                session_id="unsafe-scenario",
                template_path=template,
                scenario_id="../unsafe",
                market_path=candles,
            )
    finally:
        unsafe_client.shutdown()

    feed_scenarios = []
    for feed_price in (1.0001, 1.0002):
        chainlink = tmp_path / f"chainlink-{feed_price:.4f}.csv"
        chainlink.write_text(
            "timestamp,datetime_utc,block_number,log_index,proxy_round_id,answer,decimals,price\n"
            f"1600000000,2020-09-13T12:26:40+00:00,0,0,0,0,8,{feed_price}\n",
            encoding="utf-8",
        )
        feed_client = EvaluatorClient(EVALUATOR, work_dir=tmp_path)
        try:
            feed_ready = feed_client.open_session(
                session_id=f"feed-{feed_price:.4f}",
                template_path=template,
                scenario_id="protocol-smoke",
                market_path=candles,
                chainlink_path=chainlink,
                start_time=1_700_000_000,
                end_time=1_700_000_000 + 9 * 60,
                candle_filter=100.0,
            )
            feed_scenarios.append(feed_ready.scenarios[0].id)
        finally:
            feed_client.shutdown()
    assert feed_scenarios == ["protocol-smoke", "protocol-smoke"]

    candles.write_text("[]", encoding="utf-8")
    tampered = EvaluatorClient(EVALUATOR, work_dir=tmp_path)
    try:
        with pytest.raises(RemoteEvaluatorError, match="candles"):
            _open(tampered, template, candles, "tampered")
    finally:
        tampered.shutdown()
