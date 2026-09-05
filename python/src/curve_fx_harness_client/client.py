"""Synchronous client for arb_evaluator_ld over NDJSON."""

import io
import json
import logging
import orjson
import selectors
import subprocess
import threading
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Union

from .exceptions import (
    EvaluatorProcessError,
    IdentityMismatchError,
    ProtocolViolationError,
    RemoteEvaluatorError,
    SessionError,
)
from .models import (
    BatchResultFrame,
    CandidateSpec,
    CloseSessionFrame,
    ErrorFrame,
    EvaluateBatchFrame,
    GridReadyFrame,
    HelloFrame,
    ObservationKind,
    ObservationSpec,
    OpenSessionFrame,
    SessionClosedFrame,
    SessionReadyFrame,
    ShutdownFrame,
    YbInitialState,
)

logger = logging.getLogger("curve_fx_harness_client")


class EvaluatorClient:
    """Synchronous subprocess client managing arb_evaluator_ld in persistent serve mode."""

    def __init__(
        self,
        executable_path: Union[str, Path] = "arb_evaluator_ld",
        work_dir: Optional[Union[str, Path]] = None,
        expected_policy_id: Optional[str] = None,
        expected_policy_abi: Optional[str] = None,
        expected_policy_parameter_count: Optional[int] = None,
        launch_argv: Optional[Sequence[Union[str, Path]]] = None,
        verify_local_inputs: bool = True,
        timeout: float = 60.0,
    ):
        self.executable_path = str(executable_path)
        self.work_dir = Path(work_dir) if work_dir else Path.cwd()
        self.expected_policy_id = expected_policy_id
        self.expected_policy_abi = expected_policy_abi
        self.expected_policy_parameter_count = expected_policy_parameter_count
        self.launch_argv = (
            [str(item) for item in launch_argv]
            if launch_argv is not None
            else [self.executable_path, "serve"]
        )
        if not self.launch_argv:
            raise ValueError("launch_argv cannot be empty")
        self.verify_local_inputs = verify_local_inputs
        self.timeout = timeout

        self._proc: Optional[subprocess.Popen] = None
        self._hello: Optional[HelloFrame] = None
        self._current_session_id: Optional[str] = None
        self._request_counter = 0
        self._lock = threading.Lock()
        self._stderr_lines: List[str] = []
        self._stderr_thread: Optional[threading.Thread] = None

    def _start_unlocked(self) -> HelloFrame:
        """Internal helper to start process assuming caller holds _lock or manages state."""
        if self._proc is not None:
            raise SessionError("Evaluator process is already running")

        logger.info("Starting evaluator subprocess: %s in %s", self.launch_argv, self.work_dir)
        try:
            self._proc = subprocess.Popen(
                self.launch_argv,
                cwd=str(self.work_dir),
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                bufsize=1,  # Line-buffered
            )
        except Exception as e:
            raise EvaluatorProcessError(f"Failed to spawn evaluator executable '{self.executable_path}': {e}") from e

        # Start background stderr logging thread
        def _drain_stderr():
            assert self._proc is not None and self._proc.stderr is not None
            for line in self._proc.stderr:
                clean = line.rstrip()
                self._stderr_lines.append(clean)
                logger.debug("[evaluator:stderr] %s", clean)

        self._stderr_thread = threading.Thread(target=_drain_stderr, daemon=True)
        self._stderr_thread.start()

        # Read initial hello line
        line = self._read_line()
        try:
            data = json.loads(line)
            self._hello = HelloFrame.model_validate(data)
        except Exception as e:
            stderr_dump = "\n".join(self._stderr_lines[-20:])
            raise ProtocolViolationError(
                f"Failed to parse initial hello frame from evaluator: {e}\nLast stdout: {line}\nLast stderr: {stderr_dump}"
            ) from e

        # Verify identity requirements if configured
        identity = self._hello.evaluator_identity
        if self.expected_policy_id and identity.policy_id != self.expected_policy_id:
            raise IdentityMismatchError(
                f"Evaluator policy ID mismatch: expected '{self.expected_policy_id}', got '{identity.policy_id}'",
                expected={"policy_id": self.expected_policy_id},
                actual=identity.model_dump(),
            )
        if self.expected_policy_abi and identity.policy_abi != self.expected_policy_abi:
            raise IdentityMismatchError(
                f"Evaluator policy ABI mismatch: expected '{self.expected_policy_abi}', got '{identity.policy_abi}'",
                expected={"policy_abi": self.expected_policy_abi},
                actual=identity.model_dump(),
            )
        if (
            self.expected_policy_parameter_count is not None
            and identity.policy_parameter_count != self.expected_policy_parameter_count
        ):
            raise IdentityMismatchError(
                "Evaluator policy parameter count mismatch: "
                f"expected {self.expected_policy_parameter_count}, got {identity.policy_parameter_count}",
                expected={"policy_parameter_count": self.expected_policy_parameter_count},
                actual=identity.model_dump(),
            )

        logger.info(
            "Connected to %s (policy=%s, ABI=%s, numeric=%s, target=%s)",
            identity.build_target,
            identity.policy_id,
            identity.policy_abi,
            identity.numeric_mode,
            self.executable_path,
        )
        return self._hello

    def start(self) -> HelloFrame:
        """Start the evaluator subprocess, read the hello frame, and verify identity."""
        with self._lock:
            return self._start_unlocked()

    @property
    def hello(self) -> Optional[HelloFrame]:
        return self._hello

    @property
    def current_session_id(self) -> Optional[str]:
        return self._current_session_id

    def _next_request_id(self, prefix: str = "req") -> str:
        self._request_counter += 1
        return f"{prefix}-{self._request_counter:06d}"

    def _read_line(self) -> str:
        assert self._proc is not None and self._proc.stdout is not None
        if isinstance(self._proc.stdout, io.TextIOWrapper):
            try:
                with selectors.DefaultSelector() as selector:
                    selector.register(self._proc.stdout, selectors.EVENT_READ)
                    if not selector.select(self.timeout):
                        raise EvaluatorProcessError(
                            f"Timed out after {self.timeout:g}s waiting for evaluator output",
                            exit_code=self._proc.poll(),
                            stderr="\n".join(self._stderr_lines[-30:]),
                        )
            except (OSError, TypeError, ValueError):
                # Non-selectable platforms still use the same parser below.
                pass
        line = self._proc.stdout.readline()
        if not line:
            exit_code = self._proc.poll()
            stderr_dump = "\n".join(self._stderr_lines[-30:])
            raise EvaluatorProcessError(
                f"Evaluator process exited unexpectedly (code {exit_code}).\nStderr tail:\n{stderr_dump}",
                exit_code=exit_code,
                stderr=stderr_dump,
            )
        return line.strip()

    def _send_frame(self, frame_json: str) -> None:
        assert self._proc is not None and self._proc.stdin is not None
        try:
            self._proc.stdin.write(frame_json + "\n")
            self._proc.stdin.flush()
        except Exception as e:
            exit_code = self._proc.poll()
            stderr_dump = "\n".join(self._stderr_lines[-30:])
            raise EvaluatorProcessError(
                f"Failed to write to evaluator stdin (code {exit_code}): {e}\nStderr tail:\n{stderr_dump}",
                exit_code=exit_code,
                stderr=stderr_dump,
            ) from e

    def _transact(self, request_data: dict[str, Any]) -> dict[str, Any]:
        line_out = orjson.dumps(request_data).decode()
        self._send_frame(line_out)
        line_in = self._read_line()
        try:
            resp_data = orjson.loads(line_in)
        except Exception as e:
            raise ProtocolViolationError(f"Malformed JSON response: {line_in}") from e

        if not isinstance(resp_data, dict):
            raise ProtocolViolationError("Evaluator response must be a JSON object")
        expected_protocol = request_data.get("protocol")
        if resp_data.get("protocol") != expected_protocol:
            raise ProtocolViolationError(
                "Evaluator response protocol does not match the request: "
                f"expected {expected_protocol!r}, got {resp_data.get('protocol')!r}"
            )
        expected_request_id = request_data.get("request_id")
        if resp_data.get("request_id") != expected_request_id:
            raise ProtocolViolationError(
                "Evaluator response request_id does not match the request: "
                f"expected {expected_request_id!r}, got {resp_data.get('request_id')!r}"
            )

        if resp_data.get("type") == "error":
            err = ErrorFrame.model_validate(resp_data)
            raise RemoteEvaluatorError(
                scope=err.scope,
                error_code=err.error_code,
                message=err.message,
                details=err.details,
            )
        return resp_data

    def open_session(
        self,
        session_id: str,
        template_path: Union[str, Path],
        scenario_id: str,
        market_path: Union[str, Path],
        price_feed_path: Optional[Union[str, Path]] = None,
        pool_index: int = 0,
        n_candles: int = 0,
        start_time: int = 0,
        end_time: int = 0,
        candle_filter: float = 0.0,
        min_swap: float = 1e-6,
        max_swap: float = 1.0,
        dustswap_freq_s: int = 3600,
        user_swap_freq_s: int = 0,
        user_swap_size_frac: float = 0.01,
        user_swap_thresh: float = 0.05,
        enable_slippage_probes: bool = False,
        event_cursor: str = "scalar",
        metric_profile: str = "full_summary",
        yb_mode: str = "off",
        yb_releverage_fee: Optional[float] = None,
        yb_cash_multiplier: float = 1.0,
        yb_initial_state: Optional[Union[YbInitialState, Dict[str, Any]]] = None,
    ) -> SessionReadyFrame:
        """Open an immutable evaluation session from direct scenario inputs.

        ``yb_mode`` selects "off" (default), "active_2l" (established
        Observer2-equivalent lane), or "reference_2l" (contract-derived
        candidate lane). Enabled modes evaluate after every causal event.
        """
        with self._lock:
            if self._proc is None:
                self._start_unlocked()

            if self.verify_local_inputs:
                full_tpl = self.work_dir / template_path
                full_market = self.work_dir / market_path

                if not full_tpl.exists():
                    raise FileNotFoundError(f"Template file not found: {full_tpl}")
                if not full_market.exists():
                    raise FileNotFoundError(f"Market file not found: {full_market}")
                if price_feed_path is not None:
                    full_price_feed = self.work_dir / price_feed_path
                    if not full_price_feed.exists():
                        raise FileNotFoundError(
                            f"Price-feed file not found: {full_price_feed}"
                        )

            req_id = self._next_request_id("session")
            frame = OpenSessionFrame(
                request_id=req_id,
                session_id=session_id,
                template_path=str(template_path),
                scenario_id=scenario_id,
                market_path=str(market_path),
                price_feed_path=(
                    str(price_feed_path) if price_feed_path is not None else None
                ),
                pool_index=pool_index,
                n_candles=n_candles,
                start_time=start_time,
                end_time=end_time,
                candle_filter=candle_filter,
                min_swap=min_swap,
                max_swap=max_swap,
                dustswap_freq_s=dustswap_freq_s,
                user_swap_freq_s=user_swap_freq_s,
                user_swap_size_frac=user_swap_size_frac,
                user_swap_thresh=user_swap_thresh,
                enable_slippage_probes=enable_slippage_probes,
                event_cursor=event_cursor,
                metric_profile=metric_profile,
                yb_mode=yb_mode,
                yb_releverage_fee=yb_releverage_fee,
                yb_cash_multiplier=yb_cash_multiplier,
                yb_initial_state=yb_initial_state,
            )

            resp_data = self._transact(frame.model_dump(exclude_none=True))
            session_ready = SessionReadyFrame.model_validate(resp_data)
            self._current_session_id = session_id
            logger.info("Session '%s' ready with scenario %s", session_id, session_ready.scenario.id)
            return session_ready

    def register_grid(
        self,
        grid_id: str,
        grid: Dict[str, Any],
        *,
        session_id: Optional[str] = None,
    ) -> GridReadyFrame:
        """Compile one immutable Cartesian grid inside the active evaluator."""
        with self._lock:
            eff_session_id = session_id or self._current_session_id
            if not eff_session_id:
                raise SessionError("No active session. Call open_session() first.")
            expected = {"candidate_defaults", "axes", "axis_order", "shape"}
            if not isinstance(grid, dict) or set(grid) != expected:
                raise ValueError(
                    "grid must contain candidate_defaults, axes, axis_order, and shape"
                )
            req_id = self._next_request_id("grid")
            response = self._transact({
                "protocol": "curve_fx_eval",
                "type": "register_grid",
                "request_id": req_id,
                "session_id": eff_session_id,
                "grid_id": grid_id,
                **grid,
            })
            return GridReadyFrame.model_validate(response)

    def evaluate_batch(
        self,
        candidates: List[Union[CandidateSpec, Dict[str, Any]]],
        observation: Optional[Union[ObservationSpec, Dict[str, Any]]] = None,
        metric_fields: Optional[Sequence[str]] = None,
        metrics_format: str = "object",
        session_id: Optional[str] = None,
        trusted_candidates: bool = False,
        grid_id: Optional[str] = None,
        ranges: Optional[Sequence[Sequence[int]]] = None,
    ) -> Union[BatchResultFrame, Dict[str, Any]]:
        """Evaluate a batch of candidate pool parameter vectors over the open session."""
        with self._lock:
            eff_session_id = session_id or self._current_session_id
            if not eff_session_id:
                raise SessionError("No active session. Call open_session() first.")

            grid_mode = grid_id is not None or ranges is not None
            if grid_mode:
                if not trusted_candidates or metrics_format != "array" or not metric_fields:
                    raise ValueError(
                        "grid ranges require trusted projected array metrics"
                    )
                if not isinstance(grid_id, str) or not grid_id or not ranges or candidates:
                    raise ValueError(
                        "registered grid_id and non-empty ranges replace candidate objects"
                    )
                if any(
                    isinstance(item, (str, bytes))
                    or not isinstance(item, Sequence)
                    or len(item) != 2
                    or any(isinstance(value, bool) or not isinstance(value, int)
                           for value in item)
                    or item[0] < 0
                    or item[1] < 1
                    for item in ranges
                ):
                    raise ValueError("grid ranges must contain [start, positive count]")
                candidate_payload = None
            elif trusted_candidates:
                if metrics_format != "array" or not metric_fields:
                    raise ValueError(
                        "trusted candidates require projected array metrics"
                    )
                if any(not isinstance(candidate, dict) for candidate in candidates):
                    raise ValueError("trusted candidates must be dictionaries")
                candidate_payload = candidates
            else:
                cand_specs: List[CandidateSpec] = []
                for i, c in enumerate(candidates):
                    if isinstance(c, CandidateSpec):
                        cand_specs.append(c)
                    elif isinstance(c, dict):
                        if "ordinal" not in c:
                            c = {"ordinal": i, **c}
                        if "candidate_id" not in c:
                            c = {"candidate_id": f"cand-{i}", **c}
                        cand_specs.append(CandidateSpec.model_validate(c))
                    else:
                        raise ValueError(f"Invalid candidate type: {type(c)}")
                candidate_payload = cand_specs

            obs_spec: ObservationSpec
            if observation is None:
                obs_spec = ObservationSpec(kind=ObservationKind.SUMMARY)
            elif isinstance(observation, ObservationSpec):
                obs_spec = observation
            elif isinstance(observation, dict):
                obs_spec = ObservationSpec.model_validate(observation)
            else:
                raise ValueError(f"Invalid observation type: {type(observation)}")


            if metrics_format == "array" and not metric_fields:
                raise ValueError(
                    "metric_fields must be non-empty for array metrics"
                )

            req_id = self._next_request_id("batch")
            if trusted_candidates:
                request_data = {
                    "protocol": "curve_fx_eval",
                    "type": "evaluate_batch",
                    "request_id": req_id,
                    "session_id": eff_session_id,
                    "metric_fields": list(metric_fields or ()),
                    "metrics_format": metrics_format,
                    "observation": obs_spec.model_dump(exclude_none=True),
                }
                if grid_mode:
                    request_data["grid_id"] = grid_id
                    request_data["ranges"] = [list(item) for item in ranges or ()]
                else:
                    request_data["candidates"] = candidate_payload
            else:
                request_data = EvaluateBatchFrame(
                    request_id=req_id,
                    session_id=eff_session_id,
                    metric_fields=(
                        None if metric_fields is None else list(metric_fields)
                    ),
                    metrics_format=metrics_format,
                    observation=obs_spec,
                    candidates=candidate_payload,
                ).model_dump(exclude_none=True)

            resp_data = self._transact(request_data)
            if metrics_format == "object":
                return BatchResultFrame.model_validate(resp_data)
            if resp_data.get("type") != "batch_result":
                raise ProtocolViolationError("Expected batch_result response")
            if resp_data.get("session_id") != eff_session_id:
                raise ProtocolViolationError(
                    "Array batch response session_id does not match the request"
                )
            if resp_data.get("status") not in {
                "complete", "partial", "failed", "cancelled"
            }:
                raise ProtocolViolationError("Invalid array batch response status")
            response_fields = resp_data.get("metric_fields")
            if not isinstance(response_fields, list):
                raise ProtocolViolationError(
                    "Array batch response must echo metric_fields"
                )
            if tuple(response_fields) != tuple(metric_fields):
                raise ProtocolViolationError(
                    "Array batch response metric_fields do not match the request"
                )
            results = resp_data.get("results")
            if not isinstance(results, list):
                raise ProtocolViolationError("Array batch results must be a list")
            if any(
                not isinstance(result, dict)
                or not isinstance(result.get("metrics"), list)
                or len(result["metrics"]) != len(response_fields)
                for result in results
            ):
                raise ProtocolViolationError("Invalid metric array result")
            return resp_data

    def close_session(self, session_id: Optional[str] = None) -> SessionClosedFrame:
        """Close an active session on the evaluator."""
        with self._lock:
            eff_session_id = session_id or self._current_session_id
            if not eff_session_id:
                return SessionClosedFrame(
                    request_id="noop",
                    session_id="none",
                )

            req_id = self._next_request_id("close")
            frame = CloseSessionFrame(request_id=req_id, session_id=eff_session_id)
            resp_data = self._transact(frame.model_dump())
            self._current_session_id = None
            return SessionClosedFrame.model_validate(resp_data)

    def shutdown(self) -> None:
        """Send graceful shutdown frame and terminate child process."""
        with self._lock:
            if self._proc is not None:
                try:
                    req_id = self._next_request_id("shutdown")
                    frame = ShutdownFrame(request_id=req_id)
                    self._send_frame(json.dumps(frame.model_dump()))
                    self._proc.wait(timeout=2.0)
                except Exception:
                    self._proc.kill()
                finally:
                    self._proc = None
                    self._hello = None
                    self._current_session_id = None

    def __enter__(self) -> "EvaluatorClient":
        self.start()
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        self.shutdown()
