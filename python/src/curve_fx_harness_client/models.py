"""Pydantic models for the curve_fx_eval protocol."""

import math
from enum import Enum
from typing import Any, Dict, List, Literal, Optional, Union
from pydantic import (
    BaseModel,
    ConfigDict,
    Field,
    FiniteFloat,
    field_validator,
)


class ProtocolModel(BaseModel):
    model_config = ConfigDict(extra="forbid", allow_inf_nan=False)


class ObservationKind(str, Enum):
    SUMMARY = "summary"
    FULL_TRACE = "full_trace"


class MetricProjection(str, Enum):
    SUMMARY = "summary"
    FULL = "full"


class EvaluatorIdentity(ProtocolModel):
    harness_version: str
    pool_version: str
    policy_id: str
    policy_abi: str
    policy_parameter_count: int
    numeric_mode: Literal["double", "longdouble"]
    real_type: str
    compiler: str
    build_target: str
    ipo_enabled: bool
    native_tuning: bool


class Limits(ProtocolModel):
    max_frame_bytes: int = 4194304
    max_candidates_per_batch: int = 4096
    max_metric_values_per_batch: int = 131072
    max_materialized_batch_bytes: int = 67108864
    max_inflight_batches: int = 1


class HelloFrame(ProtocolModel):
    protocol: Literal["curve_fx_eval"] = "curve_fx_eval"
    type: Literal["hello"] = "hello"
    evaluator_identity: EvaluatorIdentity
    capabilities: List[str] = Field(
        default_factory=lambda: [
            "summary", "full_trace", "atomic_sidecars", "registered_grid_ranges"
        ]
    )
    yb_modes: List[str] = Field(default_factory=lambda: ["off", "active_2l", "reference_2l"])
    metric_schema: str = "twocrypto-summary-v1"
    metric_fields: List[str] = Field(default_factory=list)
    limits: Limits = Field(default_factory=Limits)


class OpenSessionFrame(ProtocolModel):
    protocol: Literal["curve_fx_eval"] = "curve_fx_eval"
    type: Literal["open_session"] = "open_session"
    request_id: str
    session_id: str
    template_path: str
    scenario_id: str
    market_path: str
    price_feed_path: Optional[str] = None
    pool_index: int = 0
    n_candles: int = 0
    start_time: int = 0
    end_time: int = 0
    candle_filter: FiniteFloat = 0.0
    min_swap: FiniteFloat = 1e-6
    max_swap: FiniteFloat = 1.0
    dustswap_freq_s: int = 3600
    user_swap_freq_s: int = 0
    user_swap_size_frac: FiniteFloat = 0.01
    user_swap_thresh: FiniteFloat = 0.05
    arbitrage_enabled: bool = True
    enable_slippage_probes: bool = False
    event_cursor: Literal["scalar", "exact_skip"] = "scalar"
    metric_profile: Literal["full_summary", "grid_core"] = "full_summary"
    yb_mode: Literal["off", "active_2l", "reference_2l"] = "off"
    yb_releverage_fee: FiniteFloat = 0.012
    yb_cash_multiplier: FiniteFloat = 1.0


class ScenarioInfo(ProtocolModel):
    id: str
    events_count: int
    candles_count: Optional[int] = None
    start_ts: Optional[int] = None
    end_ts: Optional[int] = None


class SessionReadyFrame(ProtocolModel):
    protocol: Literal["curve_fx_eval"] = "curve_fx_eval"
    type: Literal["session_ready"] = "session_ready"
    request_id: str
    session_id: str
    scenarios: List[ScenarioInfo]


class GridReadyFrame(ProtocolModel):
    protocol: Literal["curve_fx_eval"] = "curve_fx_eval"
    type: Literal["grid_ready"] = "grid_ready"
    request_id: str
    session_id: str
    grid_id: str
    candidate_count: int = Field(ge=1)


class ObservationSpec(ProtocolModel):
    kind: ObservationKind = ObservationKind.SUMMARY
    trace_interval: int = Field(default=1, ge=1)
    trace_actions: bool = False
    artifact_dir: Optional[str] = None





class CandidateSpec(ProtocolModel):
    ordinal: int
    candidate_id: str
    policy_params: List[FiniteFloat] = Field(default_factory=list)
    pool_overrides: Dict[str, Any] = Field(default_factory=dict)

    @field_validator("pool_overrides")
    @classmethod
    def _reject_non_finite_overrides(cls, value: Dict[str, Any]) -> Dict[str, Any]:
        pending: list[Any] = [value]
        while pending:
            item = pending.pop()
            if isinstance(item, dict):
                pending.extend(item.values())
            elif isinstance(item, (list, tuple)):
                pending.extend(item)
            elif isinstance(item, float) and not math.isfinite(item):
                raise ValueError("pool override reals must be finite binary64")
            elif isinstance(item, str) and item.strip().lower() in {
                "nan", "+nan", "-nan", "inf", "+inf", "-inf",
                "infinity", "+infinity", "-infinity",
            }:
                raise ValueError("pool override reals must be finite binary64")
        return value


class EvaluateBatchFrame(ProtocolModel):
    protocol: Literal["curve_fx_eval"] = "curve_fx_eval"
    type: Literal["evaluate_batch"] = "evaluate_batch"
    request_id: str
    session_id: str
    metric_projection: MetricProjection
    metric_fields: Optional[List[str]] = None
    metrics_format: Literal["object", "array"] = "object"
    observation: ObservationSpec = Field(default_factory=ObservationSpec)
    candidates: List[CandidateSpec]


class ArtifactRef(ProtocolModel):
    trace_path: Optional[str] = None
    actions_path: Optional[str] = None
    effective_inputs: Optional[Dict[str, Union[bool, FiniteFloat]]] = None


class ScenarioResult(ProtocolModel):
    scenario_id: str
    status: Literal["ok", "failed"] = "ok"
    error: Optional[str] = None
    metrics: Dict[str, Any] = Field(default_factory=dict)


class CandidateResult(ProtocolModel):
    ordinal: int
    candidate_id: str
    status: Literal["ok", "failed", "cancelled"] = "ok"
    error: Optional[str] = None
    metrics: Dict[str, Any] = Field(default_factory=dict)
    scenario_results: List[ScenarioResult] = Field(default_factory=list)
    artifacts: Optional[ArtifactRef] = None


class BatchResultFrame(ProtocolModel):
    protocol: Literal["curve_fx_eval"] = "curve_fx_eval"
    type: Literal["batch_result"] = "batch_result"
    request_id: str
    session_id: str
    status: Literal["complete", "partial", "failed", "cancelled"]
    metric_projection: MetricProjection = MetricProjection.SUMMARY
    results: List[CandidateResult]
    elapsed_ms: float


class ErrorFrame(ProtocolModel):
    protocol: Literal["curve_fx_eval"] = "curve_fx_eval"
    type: Literal["error"] = "error"
    request_id: str
    scope: Literal["protocol", "session", "candidate", "evaluation", "sidecar", "internal"]
    error_code: str
    message: str
    details: Dict[str, Any] = Field(default_factory=dict)


class CloseSessionFrame(ProtocolModel):
    protocol: Literal["curve_fx_eval"] = "curve_fx_eval"
    type: Literal["close_session"] = "close_session"
    request_id: str
    session_id: str


class SessionClosedFrame(ProtocolModel):
    protocol: Literal["curve_fx_eval"] = "curve_fx_eval"
    type: Literal["session_closed"] = "session_closed"
    request_id: str
    session_id: str


class ShutdownFrame(ProtocolModel):
    protocol: Literal["curve_fx_eval"] = "curve_fx_eval"
    type: Literal["shutdown"] = "shutdown"
    request_id: str
