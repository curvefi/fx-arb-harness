"""One deterministic protocol receipt for each evaluator arithmetic target."""

import json
import hashlib
import os
import subprocess
from pathlib import Path

from curve_fx_harness_client import CandidateSpec, EvaluatorClient

from test_real_evaluator import _write_inputs


def test_real_evaluator_reports_mode_and_runs_batch(tmp_path: Path) -> None:
    evaluator = os.environ["CURVE_FX_EVALUATOR"]
    expected_mode = os.environ["CURVE_FX_EXPECTED_NUMERIC_MODE"]
    expected_real_type = os.environ["CURVE_FX_EXPECTED_REAL_TYPE"]

    description = json.loads(subprocess.run(
        [evaluator, "--describe-json"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout)
    build = description["build"]
    assert build["target"] == Path(evaluator).name
    assert build["numeric_mode"] == expected_mode
    assert build["real_type"] == expected_real_type
    assert build["wire_real_digits"] == 53
    assert build["real_digits"] >= build["wire_real_digits"]

    template, _, manifest = _write_inputs(tmp_path)
    policy_params = json.loads(os.environ.get("CURVE_FX_POLICY_PARAMS", "null"))
    if policy_params is None:
        policy_params = [
            parameter["default"]
            for parameter in description["parameter_schema"]["parameters"]
            if parameter["name"].startswith("policy.")
        ]
    client = EvaluatorClient(evaluator, work_dir=tmp_path)
    try:
        hello = client.start()
        metric_schema = {
            "metric_schema": hello.metric_schema,
            "metric_fields": hello.metric_fields,
        }
        expected_metric_digest = hashlib.sha256(json.dumps(
            metric_schema, sort_keys=True, separators=(",", ":"),
        ).encode()).hexdigest()
        identity = hello.evaluator_identity
        assert identity.numeric_mode == expected_mode
        assert identity.real_type == expected_real_type
        assert identity.build_target == build["target"]
        assert len(policy_params) == identity.policy_parameter_count
        ready = client.open_session(
            session_id="numeric-mode",
            template_path=template,
            manifest_path=manifest,
        )
        assert ready.metric_schema_sha256 == expected_metric_digest
        result = client.evaluate_batch([
            CandidateSpec(
                ordinal=0,
                candidate_id="numeric-mode",
                policy_params=policy_params,
            )
        ]).results[0]
        assert result.status == "ok"
        assert result.metrics
    finally:
        client.shutdown()
