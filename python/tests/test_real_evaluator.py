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


def _write_price_feed(root: Path, contents: str) -> Path:
    path = root / "price-feed.csv"
    path.write_text(contents, encoding="utf-8")
    return path


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


def test_historical_start_apy_excludes_checkpoint_growth(tmp_path: Path) -> None:
    template, candles = _write_inputs(tmp_path, flat=True)
    document = json.loads(template.read_text())
    wad = 10**18
    # Balanced reserves imply XCP=100,000; supply=50,000 gives starting VP=2.
    state = {
        "source_timestamp": 1_700_000_000,
        "last_timestamp": 1_700_000_000,
        "balances": [str(100_000 * wad)] * 2,
        "admin_balances": ["0", "0"],
        "D": str(200_000 * wad),
        "total_supply": str(50_000 * wad),
        "price_scale": str(wad), "price_oracle": str(wad),
        "last_prices": str(wad), "virtual_price": str(2 * wad),
        "xcp_profit": str(2 * wad), "lp_xcp_profit": str(3 * wad // 2),
        "donation_shares": "0", "last_donation_release_ts": "1700000000",
        "donation_protection_expiry_ts": "0", "donation_protection_period": "3600",
        "donation_protection_lp_threshold": "0",
        "donation_protection_extension_remainder": "0",
        "donation_shares_max_ratio": str(wad // 4),
    }
    document["pools"][0]["pool"]["historical_state"] = state
    template.write_text(json.dumps(document))
    with EvaluatorClient(EVALUATOR, work_dir=tmp_path) as client:
        _open(client, template, candles, "historical", dustswap_freq_s=0)
        result = _evaluate_once(client, _policy_defaults(_description()), candidate_id="historical")
    assert result.status == "ok"
    assert result.metrics["trades"] == 0
    assert result.metrics["vp"] == 2.0
    assert result.metrics["lp_xcp_profit"] == 1.5
    assert result.metrics["apy"] == 0.0
    assert result.metrics["apy_net"] == 0.0


@pytest.mark.parametrize("price_feed_csv", [
    "ts,nav\n1699999995,1.0\n1700000600,1.001\n",
    "1699999995,1.0\n1700000600,1.001\n",
    "timestamp,a,b,c,d,e,f,price\n1699999995,0,0,0,0,0,0,1.0\n",
])
def test_identity_policy_admission_and_batch(
    tmp_path: Path, price_feed_csv: str,
) -> None:
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
    expected_count = len(policy_params)
    assert policy["parameter_count"] == expected_count == len(policy_params)
    with pytest.raises(ValueError, match="finite"):
        CandidateSpec(
            ordinal=0,
            candidate_id="non-finite",
            pool_overrides={"pool": {"A": float("inf")}},
        )

    template, candles = _write_inputs(tmp_path)
    price_feed = _write_price_feed(tmp_path, price_feed_csv)
    client = EvaluatorClient(EVALUATOR, work_dir=tmp_path)
    try:
        hello = client.start()
        identity = hello.evaluator_identity
        assert identity.numeric_mode == build["numeric_mode"]
        assert identity.real_type == build["real_type"]
        assert identity.policy_id == policy["id"]
        assert identity.policy_parameter_count == expected_count
        _open(client, template, candles, "identity", price_feed_path=price_feed)
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
    price_feed = _write_price_feed(tmp_path, "ts,price\n1699999995,1.0\n")
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
            price_feed_path=price_feed,
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
        assert trace[0]["p_price_feed"] == pytest.approx(1.0)
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
            price_feed_path=price_feed,
            dustswap_freq_s=0,
            user_swap_freq_s=180,
            user_swap_size_frac=0.01,
            user_swap_thresh=1.0,
            enable_slippage_probes=True,
        )
        user_result = _evaluate_once(
            client,
            policy_params,
            candidate_id="user-scheduled",
            metric_fields=("tw_real_slippage_1pct",),
            observation=ObservationSpec(
                kind="full_trace",
                trace_interval=1,
                trace_actions=True,
                artifact_dir="user-sidecar",
            ),
        )
        assert user_result.artifacts is not None
        assert user_result.metrics["tw_real_slippage_1pct"] != -1.0
        user_trace = json.loads(
            (tmp_path / user_result.artifacts.trace_path).read_text(encoding="utf-8")
        )
        by_time = {row["t"]: row for row in user_trace}
        assert user_trace[-1]["n_rebalances"] > 0
        before = by_time[1_700_000_175]
        after = by_time[1_700_000_185]
        fair_tvl = before["token0"] + after["p_cex"] * before["token1"]
        expected_dx = fair_tvl * 0.01 * 180 / 86_400
        assert after["token0"] - before["token0"] == pytest.approx(expected_dx)
        user_actions = [row for row in json.loads(
            (tmp_path / user_result.artifacts.actions_path).read_text(encoding="utf-8")
        ) if row.get("actor") == "user"]
        assert user_actions
        first_user = user_actions[0]
        assert (first_user["type"], first_user["i"], first_user["j"]) == ("exchange", 0, 1)
        assert first_user["dx"] == pytest.approx(expected_dx)
        assert before["token1"] - after["token1"] == pytest.approx(
            first_user["dy_after_fee"]
        )
        assert first_user["dy_after_fee"] > 0
        assert first_user["fee_tokens"] > 0
        assert "profit_coin0" not in first_user

    with EvaluatorClient(EVALUATOR, work_dir=tmp_path) as client:
        _open(
            client,
            template,
            candles,
            "user-rejected",
            price_feed_path=price_feed,
            dustswap_freq_s=0,
            user_swap_freq_s=180,
            user_swap_size_frac=0.01,
            user_swap_thresh=0.0,
        )
        rejected = _evaluate_once(
            client,
            policy_params,
            candidate_id="user-rejected",
            observation=ObservationSpec(
                kind="full_trace", trace_interval=1, trace_actions=True,
                artifact_dir="user-rejected-sidecar",
            ),
        )
        assert rejected.artifacts is not None
        rejected_actions = json.loads(
            (tmp_path / rejected.artifacts.actions_path).read_text(encoding="utf-8")
        )
        assert not [row for row in rejected_actions if row.get("actor") == "user"]

    for mode in ("active_2l", "reference_2l"):
        with EvaluatorClient(EVALUATOR, work_dir=tmp_path) as client:
            _open(
                client,
                template,
                candles,
                f"yb-{mode}",
                price_feed_path=price_feed,
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
