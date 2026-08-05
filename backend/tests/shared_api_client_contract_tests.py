#!/usr/bin/env python3
"""Focused source contracts for the shared browser API client."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CLIENT = (ROOT / "frontend" / "api-client.js").read_text(encoding="utf-8")
CONFIG = (ROOT / "frontend" / "config.js").read_text(encoding="utf-8")
HEALTH_CORS = (ROOT / "backend" / "src" / "health_status.cpp").read_text(encoding="utf-8")
ACTIVE_CORS = (ROOT / "backend" / "src" / "http_security.cpp").read_text(encoding="utf-8")
SECURITY = (ROOT / "backend" / "src" / "security_hardening.cpp").read_text(encoding="utf-8")


def require(source: str, needle: str, description: str) -> None:
    if needle not in source:
        raise AssertionError(f"Missing {description}: {needle}")


def script_index(name: str) -> int:
    marker = f"'{name}'"
    index = CONFIG.find(marker)
    if index < 0:
        raise AssertionError(f"Missing shared script: {name}")
    return index


if not script_index("api-client.js") < script_index("authoritative-data.js"):
    raise AssertionError("API client must load before authoritative data")
require(CLIENT, "const DEFAULT_TIMEOUT_MS = 12000", "default request timeout")
require(CLIENT, "const SAFE_RETRY_METHODS = new Set(['GET', 'HEAD', 'OPTIONS'])", "safe-method retry allowlist")
require(CLIENT, "const RETRYABLE_STATUSES = new Set([408, 425, 502, 503, 504])", "transient status allowlist")
require(CLIENT, "if (!SAFE_RETRY_METHODS.has(methodName(method))) return false", "unsafe-method retry rejection")
require(CLIENT, "baseHeaders.set('X-Request-ID', requestId)", "outgoing request identifier")
require(CLIENT, "externalAborted = true", "caller cancellation tracking")
require(CLIENT, "delete requestInit.cffTimeoutMs", "custom option removal before native fetch")
require(CLIENT, "normalizeApiError", "normalized API errors")
require(CLIENT, "correlationId: requestId", "correlation identifier alias")
require(CLIENT, "root.apiRequest = wrapped", "legacy authenticated request adapter")
require(CLIENT, "root.fetchJson = wrapped", "authentication request adapter")
require(CLIENT, "root.mutationErrorMessage = wrapped", "request reference mutation messages")
for cors_source in (HEALTH_CORS, ACTIVE_CORS):
    require(cors_source, "Authorization, Content-Type, X-Request-ID, Idempotency-Key", "request and idempotency CORS allow-headers")
    require(cors_source, '"GET, POST, PUT, PATCH, DELETE, OPTIONS"', "complete API method CORS allowlist")
require(SECURITY, '"X-CFF-Request-Id, Retry-After, X-CFF-Invite-Email"', "request ID response exposure")
require(SECURITY, 'for (const auto *header : {"rndr-id", "x-request-id"})', "backend request ID intake")
require(SECURITY, 'resp->addHeader("X-CFF-Request-Id", currentRequestId)', "backend request ID echo")

print("shared API client source contracts passed")
