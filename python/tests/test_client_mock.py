import io
import json
import math

import pytest
from unittest.mock import MagicMock, patch
from typing import Any, Literal

from curve_fx_harness_client.client import EvaluatorClient
from curve_fx_harness_client.exceptions import IdentityMismatchError, ProtocolViolationError
from pydantic import BaseModel, ConfigDict, ValidationError
from curve_fx_harness_client.models import CandidateSpec, EvaluateBatchFrame, HelloFrame


class LegacyLimits(BaseModel):
    model_config = ConfigDict(extra="forbid")
    max_frame_bytes: int
    max_candidates_per_batch: int | None = None
    max_inflight_batches: int


class CurrentHelloFrame(BaseModel):
    model_config = ConfigDict(extra="forbid")
    protocol: Literal["curve_fx_eval"]
    type: Literal["hello"]
    evaluator_identity: dict[str, Any]
    capabilities: list[str]
    yb_modes: list[str] = ["off", "active_2l", "reference_2l"]
    metric_schema: str
    metric_fields: list[str]
    limits: LegacyLimits


class CurrentSessionReadyFrame(BaseModel):
    model_config = ConfigDict(extra="forbid")
    protocol: Literal["curve_fx_eval"]
    type: Literal["session_ready"]
    request_id: str
    session_id: str
    scenarios: list[dict[str, Any]]


@pytest.fixture
def mock_hello_data():
    return {
        "protocol": "curve_fx_eval",
        "type": "hello",
        "evaluator_identity": {
            "harness_version": "1.0.0",
            "pool_version": "1.0.0",
            "policy_id": "challenge_v1",
            "policy_abi": "twocrypto_policy_v1",
            "policy_parameter_count": 0,
            "numeric_mode": "longdouble",
            "real_type": "long double",
            "compiler": "clang",
            "build_target": "arb_evaluator_ld",
            "ipo_enabled": False,
            "native_tuning": False,
        },
        "capabilities": ["summary", "full_trace"],
        "metric_schema": "twocrypto-summary-v1",
        "metric_fields": ["vp", "trades"],
        "limits": {
            "max_frame_bytes": 4194304,
            "max_inflight_batches": 1,
        },
    }


@pytest.fixture
def mock_session_ready_data():
    return {
        "protocol": "curve_fx_eval",
        "type": "session_ready",
        "request_id": "session-000001",
        "session_id": "test_session",
        "scenarios": [{"id": "scen_1", "events_count": 1000}],
    }


def test_client_identity_mismatch(mock_hello_data, tmp_path):
    client = EvaluatorClient(
        executable_path="arb_evaluator_ld",
        work_dir=tmp_path,
        expected_policy_id="expected_other_policy",
    )

    mock_proc = MagicMock()
    mock_proc.stdout.readline.return_value = json.dumps(mock_hello_data) + "\n"
    mock_proc.stderr = io.StringIO("")
    mock_proc.poll.return_value = None

    with patch("subprocess.Popen", return_value=mock_proc):
        with pytest.raises(IdentityMismatchError, match="policy ID mismatch"):
            client.start()


def test_current_responses_match_strict_models(
    mock_hello_data, mock_session_ready_data
) -> None:
    CurrentHelloFrame.model_validate(mock_hello_data)
    CurrentSessionReadyFrame.model_validate(mock_session_ready_data)


def test_open_session_on_fresh_client_no_deadlock(mock_hello_data, mock_session_ready_data, tmp_path):
    tpl = tmp_path / "template.json"
    tpl.write_text("{}", encoding="utf-8")
    market = tmp_path / "candles.json"
    market.write_text("[]", encoding="utf-8")

    client = EvaluatorClient(executable_path="arb_evaluator_ld", work_dir=tmp_path)

    mock_proc = MagicMock()
    mock_proc.stdout.readline.side_effect = [
        json.dumps(mock_hello_data) + "\n",
        json.dumps(mock_session_ready_data) + "\n",
    ]
    mock_proc.stderr = io.StringIO("")
    mock_proc.poll.return_value = None

    with patch("subprocess.Popen", return_value=mock_proc):
        session_ready = client.open_session(
            session_id="test_session",
            template_path="template.json",
            scenario_id="scen_1",
            market_path="candles.json",
        )
        assert session_ready.session_id == "test_session"
        assert len(session_ready.scenarios) == 1


def test_remote_launch_uses_canonical_frames_without_local_files(
    mock_hello_data, mock_session_ready_data, tmp_path
):
    launch = ["ssh", "blade-a1", "/shared/arb_evaluator_ld", "serve"]
    client = EvaluatorClient(
        executable_path="/shared/arb_evaluator_ld",
        work_dir=tmp_path,
        launch_argv=launch,
        verify_local_inputs=False,
        expected_policy_id="challenge_v1",
        expected_policy_abi="twocrypto_policy_v1",
        expected_policy_parameter_count=0,
    )

    mock_proc = MagicMock()
    mock_proc.stdout.readline.side_effect = [
        json.dumps(mock_hello_data) + "\n",
        json.dumps(mock_session_ready_data) + "\n",
    ]
    mock_proc.stderr = io.StringIO("")
    mock_proc.poll.return_value = None

    with patch("subprocess.Popen", return_value=mock_proc) as popen:
        client.open_session(
            session_id="test_session",
            template_path="/shared/template.json",
            scenario_id="scen_1",
            market_path="/shared/candles.json",
            chainlink_path="/shared/chainlink.csv",
        )

    assert popen.call_args.args[0] == launch
    request = json.loads(mock_proc.stdin.write.call_args_list[0].args[0])
    assert request["type"] == "open_session"
    assert request["template_path"] == "/shared/template.json"
    assert request["scenario_id"] == "scen_1"
    assert request["market_path"] == "/shared/candles.json"
    assert request["chainlink_path"] == "/shared/chainlink.csv"


@pytest.mark.parametrize(
    "response",
    [
        {
            "protocol": "curve_fx_eval",
            "type": "session_ready",
            "request_id": "session-stale",
            "session_id": "test_session",
            "scenarios": [{"id": "scen_1", "events_count": 1000}],
        },
        {
            "protocol": "curve_fx_eval",
            "type": "error",
            "request_id": "session-stale",
            "scope": "session",
            "error_code": "STALE_RESPONSE",
            "message": "stale response",
            "details": {},
        },
    ],
    ids=["success", "error"],
)
def test_open_session_rejects_wrong_response_request_id(
    mock_hello_data,
    response,
    tmp_path,
):
    (tmp_path / "template.json").write_text("{}", encoding="utf-8")
    (tmp_path / "candles.json").write_text("[]", encoding="utf-8")
    client = EvaluatorClient(executable_path="arb_evaluator_ld", work_dir=tmp_path)

    mock_proc = MagicMock()
    mock_proc.stdout.readline.side_effect = [
        json.dumps(mock_hello_data) + "\n",
        json.dumps(response) + "\n",
    ]
    mock_proc.stderr = io.StringIO("")
    mock_proc.poll.return_value = None

    with patch("subprocess.Popen", return_value=mock_proc):
        with pytest.raises(ProtocolViolationError, match="request_id"):
            client.open_session(
                session_id="test_session",
                template_path="template.json",
                scenario_id="scen_1",
                market_path="candles.json",
            )


def test_evaluate_batch_requires_metric_projection() -> None:
    with pytest.raises(ValidationError, match="metric_projection"):
        EvaluateBatchFrame(
            request_id="batch-1",
            session_id="session-1",
            candidates=[CandidateSpec(ordinal=0, candidate_id="candidate-0")],
        )


@pytest.mark.parametrize("value", [math.nan, math.inf, -math.inf, "nan", "inf"])
def test_candidate_rejects_non_finite_binary64_inputs(value: Any) -> None:
    with pytest.raises(ValidationError, match="finite"):
        CandidateSpec(
            ordinal=0,
            candidate_id="candidate-0",
            policy_params=[value],
        )
    with pytest.raises(ValidationError, match="finite"):
        CandidateSpec(
            ordinal=0,
            candidate_id="candidate-0",
            pool_overrides={"pool": {"A": value}},
        )


@pytest.mark.parametrize("field", ["ipo_enabled", "native_tuning"])
def test_hello_requires_identity_build_fields(mock_hello_data, field) -> None:
    del mock_hello_data["evaluator_identity"][field]
    with pytest.raises(ValidationError, match=field):
        HelloFrame.model_validate(mock_hello_data)
