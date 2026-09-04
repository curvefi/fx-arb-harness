# curve-fx-harness-client

Typed Python client and protocol models for the `curve_fx_eval` simulation harness.

## Installation
```bash
uv pip install -e .
```

## Quick Start
```python
from curve_fx_harness_client import EvaluatorClient, CandidateSpec

with EvaluatorClient(executable_path="../build/native/arb_evaluator_ld") as client:
    client.open_session(
        session_id="btc-2024",
        template_path="templates/pool_btc.json",
        scenario_id="btc-2024",
        market_path="markets/btc.json",
        price_feed_path="markets/btc_reference_prices.csv",
    )
    result = client.evaluate_batch(
        candidates=[
            CandidateSpec(
                ordinal=0,
                candidate_id="c0",
                policy_params=[],
            )
        ],
    )
    print(result.results[0].metrics)
```
