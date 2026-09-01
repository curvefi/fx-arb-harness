import json
import os
import subprocess
from pathlib import Path

import pytest

from curve_fx_harness_client import CandidateSpec, EvaluatorClient
from curve_fx_harness_client.models import ObservationSpec


EVALUATOR = os.environ.get("CURVE_FX_EVALUATOR")
pytestmark = pytest.mark.skipif(
    not EVALUATOR,
    reason="CURVE_FX_EVALUATOR is required for the protocol contract tests",
)


def _write_inputs(root: Path, *, flat: bool = False) -> tuple[Path, Path]:
    template = root / "template.json"
    template.write_text(
        json.dumps({
            "pools": [{
                "tag": "protocol_contract",
                "pool": {
                    "initial_liquidity": [
                        "100000000000000000000000",
                        "100000000000000000000000",
                    ],
                    "A": "500000.0",
                    "gamma": "100000000000000",
                    "mid_fee": "1000000.0",
                    "out_fee": "20000000.0",
                    "fee_gamma": "30000000000000123",
                    "adjustment_step_min": "100000000",
                    "adjustment_step_max": "5000000000000000",
                    "ma_time": "865",
                    "reserved_profit_fraction": "3400000123",
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
                    "gas_coin0": 1_000_000_000 if flat else 0,
                    "use_volume_cap": False,
                    "volume_cap_mult": 1,
                },
            }]
        }, sort_keys=True),
        encoding="utf-8",
    )
    candles = root / "candles.json"
    rows = []
    for index in range(24):
        price = 1.0 if flat else 1.0 + ((index % 5) - 2) * 0.0001
        rows.append([
            1_700_000_000 + index * 60,
            price,
            price + 0.0001,
            price - 0.0001,
            price,
            1_000_000.0,
        ])
    candles.write_text(json.dumps(rows, separators=(",", ":")), encoding="utf-8")
    return template, candles


def _description() -> dict[str, object]:
    assert EVALUATOR is not None
    return json.loads(subprocess.run(
        [EVALUATOR, "--describe-json"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout)


def _policy_defaults(description: dict[str, object]) -> list[float]:
    parameters = description["parameter_schema"]["parameters"]
    return [
        parameter["default"]
        for parameter in parameters
        if parameter["name"].startswith("policy.")
    ]


def _open(
    client: EvaluatorClient,
    template: Path,
    candles: Path,
    session_id: str,
    **kwargs,
):
    return client.open_session(
        session_id=session_id,
        template_path=template,
        scenario_id="protocol-contract",
        market_path=candles,
        start_time=1_700_000_000,
        end_time=1_700_000_000 + 23 * 60,
        candle_filter=100.0,
        **kwargs,
    )


def _evaluate_once(
    client: EvaluatorClient,
    policy_params: list[float],
    *,
    candidate_id: str,
    ordinal: int = 0,
    **kwargs,
):
    return client.evaluate_batch([
        CandidateSpec(
            ordinal=ordinal,
            candidate_id=candidate_id,
            policy_params=policy_params,
        )
    ], **kwargs).results[0]


def test_identity_policy_admission_and_batch(tmp_path: Path) -> None:
    assert EVALUATOR is not None
    description = _description()
    build = description["build"]
    expected_mode = os.environ.get("CURVE_FX_EXPECTED_NUMERIC_MODE")
    expected_real_type = os.environ.get("CURVE_FX_EXPECTED_REAL_TYPE")
    if expected_mode:
        assert build["numeric_mode"] == expected_mode
    if expected_real_type:
        assert build["real_type"] == expected_real_type
    assert build["target"] == Path(EVALUATOR).name
    assert build["wire_real_type"] == "IEEE-754 binary64"
    assert build["wire_real_digits"] == 53
    assert build["real_digits"] >= build["wire_real_digits"]

    policy_params = _policy_defaults(description)
    policy = description["policy"]
    expected_count = 0 if policy["id"] == "none" else 6
    assert policy["parameter_count"] == expected_count == len(policy_params)
    with pytest.raises(ValueError, match="finite"):
        CandidateSpec(
            ordinal=0,
            candidate_id="non-finite",
            pool_overrides={"pool": {"A": float("inf")}},
        )

    template, candles = _write_inputs(tmp_path)
    client = EvaluatorClient(EVALUATOR, work_dir=tmp_path)
    try:
        hello = client.start()
        identity = hello.evaluator_identity
        assert identity.numeric_mode == build["numeric_mode"]
        assert identity.real_type == build["real_type"]
        assert identity.policy_id == policy["id"]
        assert identity.policy_parameter_count == expected_count
        _open(client, template, candles, "identity")
        result = _evaluate_once(
            client,
            policy_params,
            candidate_id="identity",
        )
        assert result.status == "ok"
        assert result.metrics
    finally:
        client.shutdown()


def test_profiles_and_registered_range_match_public_results(tmp_path: Path) -> None:
    assert EVALUATOR is not None
    policy_params = _policy_defaults(_description())
    template, candles = _write_inputs(tmp_path)
    fields = (
        "vp",
        "apy_net",
        "apy_net_robust_90d",
        "avg_rel_price_diff",
        "detach_energy_ungated",
        "trades",
        "n_rebalances",
    )
    override = {
        "pool": {
            "A": "5.000000000000001",
            "mid_fee": "0.001",
            "out_fee": "0.001",
            "fee_gamma": "0.030000000000000123",
            "reserved_profit_fraction": "0.3400000123",
        }
    }

    with EvaluatorClient(EVALUATOR, work_dir=tmp_path) as scalar:
        _open(scalar, template, candles, "scalar", metric_profile="full_summary")
        direct = scalar.evaluate_batch([
            CandidateSpec(
                ordinal=4,
                candidate_id="direct",
                policy_params=policy_params,
                pool_overrides=override,
            )
        ], metric_fields=fields, metrics_format="array")["results"][0]
        scalar.register_grid("one-point", {
            "candidate_defaults": {
                "policy_params": policy_params,
                "pool": override["pool"],
            },
            "axes": {},
            "axis_order": [],
            "shape": [],
        })
        ranged = scalar.evaluate_batch(
            [],
            metric_fields=fields,
            metrics_format="array",
            trusted_candidates=True,
            grid_id="one-point",
            ranges=((0, 1),),
        )["results"][0]
        assert ranged["ordinal"] == 0
        assert ranged["candidate_id"] == "p00000000"
        assert ranged["metrics"] == direct["metrics"]

    with EvaluatorClient(EVALUATOR, work_dir=tmp_path) as skipped:
        _open(
            skipped,
            template,
            candles,
            "exact-skip",
            event_cursor="exact_skip",
            metric_profile="grid_core",
        )
        exact = skipped.evaluate_batch([
            CandidateSpec(
                ordinal=4,
                candidate_id="exact-skip",
                policy_params=policy_params,
                pool_overrides=override,
            )
        ], metric_fields=fields).results[0]
        assert exact.metrics == dict(zip(fields, direct["metrics"]))


def test_scheduling_yb_and_atomic_sidecars(tmp_path: Path) -> None:
    assert EVALUATOR is not None
    policy_params = _policy_defaults(_description())
    template, candles = _write_inputs(tmp_path, flat=True)
    observation = ObservationSpec(
        kind="full_trace",
        trace_interval=1,
        trace_actions=True,
        artifact_dir="sidecars",
    )

    client = EvaluatorClient(EVALUATOR, work_dir=tmp_path)
    try:
        hello = client.start()
        assert "atomic_sidecars" in hello.capabilities
        _open(
            client,
            template,
            candles,
            "scheduling",
            dustswap_freq_s=120,
        )
        scheduled = _evaluate_once(
            client,
            policy_params,
            candidate_id="scheduled",
            observation=observation,
        )
        assert scheduled.status == "ok"
        assert scheduled.artifacts is not None
        trace_path = tmp_path / scheduled.artifacts.trace_path
        actions_path = tmp_path / scheduled.artifacts.actions_path
        trace = json.loads(trace_path.read_text(encoding="utf-8"))
        actions = json.loads(actions_path.read_text(encoding="utf-8"))
        tick_timestamps = [row["ts"] for row in actions if row["type"] == "tick"]
        assert tick_timestamps[:2] == [1_700_000_125, 1_700_000_245]
        assert all(
            later - earlier == 120
            for earlier, later in zip(tick_timestamps, tick_timestamps[1:])
        )
        assert {path.name for path in (tmp_path / "sidecars").iterdir()} == {
            trace_path.name,
            actions_path.name,
        }

        effective = scheduled.artifacts.effective_inputs
        assert effective["pool.fee_gamma"] == pytest.approx(0.030000000000000123)
        assert effective["pool.reserved_profit_fraction"] == pytest.approx(0.3400000123)
    finally:
        client.shutdown()

    with EvaluatorClient(EVALUATOR, work_dir=tmp_path) as client:
        _open(
            client,
            template,
            candles,
            "user-scheduling",
            dustswap_freq_s=0,
            user_swap_freq_s=180,
            user_swap_size_frac=0.01,
            user_swap_thresh=1.0,
        )
        user_result = _evaluate_once(
            client,
            policy_params,
            candidate_id="user-scheduled",
            observation=ObservationSpec(
                kind="full_trace",
                trace_interval=1,
                artifact_dir="user-sidecar",
            ),
        )
        assert user_result.artifacts is not None
        user_trace = json.loads(
            (tmp_path / user_result.artifacts.trace_path).read_text(encoding="utf-8")
        )
        by_time = {row["t"]: row for row in user_trace}
        assert by_time[1_700_000_185]["xp_0"] != by_time[1_700_000_175]["xp_0"]

    for mode in ("active_2l", "reference_2l"):
        with EvaluatorClient(EVALUATOR, work_dir=tmp_path) as client:
            _open(
                client,
                template,
                candles,
                f"yb-{mode}",
                yb_mode=mode,
                yb_releverage_fee=0.013,
                yb_cash_multiplier=3.0,
            )
            result = _evaluate_once(
                client,
                policy_params,
                candidate_id=f"yb-{mode}",
                metric_fields=("yb_apy", "yb_fee"),
            )
            assert result.status == "ok"
            assert result.metrics["yb_apy"] != -1.0
            assert result.metrics["yb_fee"] == pytest.approx(0.013)
