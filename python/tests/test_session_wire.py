import pytest

from curve_fx_harness_client import EvaluatorClient
from curve_fx_harness_client.exceptions import ProtocolViolationError


def test_candle_sessions_omit_inactive_extensions_and_depth_keeps_its_controls(monkeypatch):
    cases = [
        ({"market_path": "candles.json"}, {}),
        ({"cex_depth_path": "book.npz", "event_mode": "depth"},
         {"cex_depth_max_age_s": 30, "event_mode": "depth"}),
        ({"cex_depth_path": "book.npz", "cex_depth_max_age_s": 0,
          "event_mode": "depth", "actor_timing_mode": "minute_sequential"},
         {"cex_depth_max_age_s": 0, "event_mode": "depth",
          "actor_timing_mode": "minute_sequential"}),
    ]
    for options, expected in cases:
        client = EvaluatorClient()
        wire = {}

        def transact(request):
            wire.update(request)
            return {
                "protocol": "curve_fx_eval", "type": "session_ready",
                "request_id": request["request_id"], "session_id": request["session_id"],
                "scenario": {"id": "case", "events_count": 2},
            }

        monkeypatch.setattr(client, "_start_unlocked", lambda: None)
        monkeypatch.setattr(client, "_transact", transact)
        client.open_session("replay", "template.json", "case", yb_mode="active_2l", **options)
        controls = {key: wire[key] for key in
                    ("cex_depth_max_age_s", "actor_timing_mode", "event_mode") if key in wire}
        assert controls == expected
        assert wire["yb_mode"] == "active_2l"


def test_array_results_validate_values_at_the_client_boundary(monkeypatch):
    client = EvaluatorClient()
    row = {"ordinal": 0, "candidate_id": "p00000000", "status": "ok", "metrics": [1.25]}
    response = {"type": "batch_result", "session_id": "s", "status": "complete",
                "metric_fields": ["score"], "results": [row]}
    monkeypatch.setattr(client, "_transact", lambda request: response)
    request = dict(session_id="s", grid_id="g", ranges=[(0, 1)],
                   metric_fields=["score"], metrics_format="array", trusted_candidates=True)
    assert client.evaluate_batch([], **request)["results"] == [row]
    for update in ({"metrics": [float("nan")]}, {"ordinal": False}, {"status": "unknown"}):
        response["results"] = [{**row, **update}]
        with pytest.raises(ProtocolViolationError, match="Invalid metric array result"):
            client.evaluate_batch([], **request)
