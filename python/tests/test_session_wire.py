from curve_fx_harness_client import EvaluatorClient


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
        client = EvaluatorClient(verify_local_inputs=False)
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
