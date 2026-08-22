"""Exceptions for the curve_fx_eval harness client."""

from typing import Any, Dict, Optional


class HarnessError(Exception):
    """Base exception for all curve_fx_harness_client errors."""
    pass


class ProtocolViolationError(HarnessError):
    """Raised when an NDJSON frame violates protocol framing or envelope rules."""
    pass


class EvaluatorProcessError(HarnessError):
    """Raised when the evaluator subprocess fails, exits unexpectedly, or logs fatal errors."""
    def __init__(self, message: str, exit_code: Optional[int] = None, stderr: Optional[str] = None):
        super().__init__(message)
        self.exit_code = exit_code
        self.stderr = stderr


class IdentityMismatchError(HarnessError):
    """Raised when the running binary's identity or policy ABI does not match expectations."""
    def __init__(self, message: str, expected: Dict[str, Any], actual: Dict[str, Any]):
        super().__init__(message)
        self.expected = expected
        self.actual = actual


class SessionError(HarnessError):
    """Raised when session creation or session lifecycle state fails."""
    pass






class RemoteEvaluatorError(HarnessError):
    """Raised when the evaluator returns an explicit error frame."""
    def __init__(self, scope: str, error_code: str, message: str, details: Optional[Dict[str, Any]] = None):
        super().__init__(f"[{scope}:{error_code}] {message}")
        self.scope = scope
        self.error_code = error_code
        self.error_message = message
        self.details = details or {}
