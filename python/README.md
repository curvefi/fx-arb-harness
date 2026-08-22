# curve-fx-harness-client

Typed Python client and protocol models for the `curve_fx_eval` simulation harness.

## Installation
```bash
uv pip install -e .
```

## Quick Start
```python
from curve_fx_harness_client import EvaluatorClient, CandidateSpec, MetricProjection

with EvaluatorClient(executable_path="../build/arb_evaluator_ld") as client:
    client.open_session(
        session_id="btc-2024",
        template_path="templates/pool_btc.json",
        scenario_id="btc-2024",
        market_path="markets/btc.json",
        chainlink_path="markets/btc_chainlink.json",
    )
    result = client.evaluate_batch(
        candidates=[
            CandidateSpec(
                ordinal=0,
                candidate_id="c0",
                policy_params=[3600.0, 86400.0, 1.0, 10.0, 0.0],
            )
        ],
        metric_projection=MetricProjection.SUMMARY,
    )
    print(result.results[0].metrics)
```
