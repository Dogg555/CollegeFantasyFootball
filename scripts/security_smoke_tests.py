#!/usr/bin/env python3
"""Focused production security smoke tests for the deployed CFF API."""

from __future__ import annotations

import json
import os
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from typing import Any


BASE_URL = os.environ.get("CFF_API_BASE_URL", "http://127.0.0.1:8080").rstrip("/")
ALLOWED_ORIGIN = os.environ.get("CFF_SECURITY_TEST_ORIGIN", "https://frontend.example.test")


@dataclass
class Response:
    status: int
    headers: dict[str, str]
    body: bytes

    def json(self) -> Any:
        return json.loads(self.body.decode("utf-8")) if self.body else None


def request(
    path: str,
    *,
    method: str = "GET",
    payload: Any | None = None,
    raw_body: bytes | None = None,
    headers: dict[str, str] | None = None,
) -> Response:
    final_headers = dict(headers or {})
    body = raw_body
    if payload is not None:
        body = json.dumps(payload).encode("utf-8")
        final_headers.setdefault("Content-Type", "application/json")
    req = urllib.request.Request(
        f"{BASE_URL}{path}",
        data=body,
        headers=final_headers,
        method=method,
    )
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            return Response(resp.status, {k.lower(): v for k, v in resp.headers.items()}, resp.read())
    except urllib.error.HTTPError as error:
        return Response(error.code, {k.lower(): v for k, v in error.headers.items()}, error.read())


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def wait_for_health() -> None:
    for _ in range(45):
        try:
            response = request("/health")
            if response.status == 200 and response.json().get("status") == "ok":
                return
        except Exception:
            pass
        time.sleep(2)
    raise AssertionError("API did not become healthy")


def main() -> int:
    wait_for_health()

    health = request("/health")
    require(health.status == 200, "health endpoint must return 200")
    health_json = health.json()
    require(set(health_json) == {"status", "service", "database"}, "health response exposed internal configuration")
    require(health_json["database"] == "ok", "database must be healthy")
    require(health.headers.get("x-content-type-options") == "nosniff", "nosniff header missing")
    require(health.headers.get("x-frame-options") == "DENY", "frame protection header missing")
    require(health.headers.get("cache-control") == "no-store", "API cache policy missing")
    require("server" not in health.headers, "Server header must be disabled")

    blocked_origin = request("/api/health", headers={"Origin": "https://attacker.invalid"})
    require(blocked_origin.status == 403, "disallowed CORS origin was not rejected")
    require(blocked_origin.json().get("code") == "origin_not_allowed", "unexpected CORS rejection response")

    allowed_origin = request("/api/health", headers={"Origin": ALLOWED_ORIGIN})
    require(allowed_origin.status == 200, "allowed CORS origin should succeed")
    require(allowed_origin.headers.get("access-control-allow-origin") == ALLOWED_ORIGIN, "allowed origin header missing")

    weak = request(
        "/api/auth/signup",
        method="POST",
        payload={"email": "weak-password@example.test", "password": "password123"},
        headers={"Origin": ALLOWED_ORIGIN},
    )
    require(weak.status == 400, "weak password should be rejected")
    require(weak.json().get("code") == "weak_password", "weak password rejection code missing")
    require(
        weak.headers.get("access-control-allow-origin") == ALLOWED_ORIGIN,
        "allowed-origin auth validation errors must include CORS headers",
    )

    wrong_type = request(
        "/api/auth/signup",
        method="POST",
        raw_body=b'{"email":"type@example.test","password":"Correct-Horse-Battery-Staple-2026!"}',
        headers={"Content-Type": "text/plain"},
    )
    require(wrong_type.status == 415, "non-JSON mutation should be rejected")

    oversized_payload = {
        "email": "large@example.test",
        "password": "Correct-Horse-Battery-Staple-2026!",
        "padding": "x" * 9000,
    }
    oversized = request("/api/auth/signup", method="POST", payload=oversized_payload)
    require(oversized.status == 413, "oversized authentication request should be rejected")

    unique_email = f"security-smoke-{int(time.time())}@example.test"
    signup = request(
        "/api/auth/signup",
        method="POST",
        payload={"email": unique_email, "password": "Correct-Horse-Battery-Staple-2026!"},
    )
    require(signup.status == 201, f"strong signup failed with {signup.status}")
    token = signup.json().get("token")
    require(isinstance(token, str) and len(token) >= 64, "signup did not issue a strong session token")

    auth_headers = {"Authorization": f"Bearer {token}"}
    validate = request("/api/auth/validate", headers=auth_headers)
    require(validate.status == 200 and validate.json().get("valid") is True, "new token did not validate")

    logout = request("/api/auth/logout", method="POST", payload={}, headers=auth_headers)
    require(logout.status == 200, "logout failed")
    revoked = request("/api/auth/validate", headers=auth_headers)
    require(revoked.status == 401, "logged-out token remained valid")

    admin_statuses = []
    for _ in range(4):
        result = request(
            "/api/admin/ingest/cfbd",
            method="POST",
            payload={},
            headers={"Authorization": "Bearer invalid-admin-token"},
        )
        admin_statuses.append(result.status)
    require(admin_statuses[-1] == 429, f"admin endpoint was not throttled: {admin_statuses}")

    live_admin_statuses = []
    for _ in range(5):
        result = request(
            "/api/admin/ingest/cfbd/live",
            method="POST",
            payload={},
            headers={"Authorization": "Bearer invalid-live-admin-token"},
        )
        live_admin_statuses.append(result.status)
    require(live_admin_statuses[:4] == [403, 403, 403, 403],
            f"live ingestion cadence was throttled too early: {live_admin_statuses}")
    require(live_admin_statuses[-1] == 429,
            f"live ingestion endpoint was not bounded: {live_admin_statuses}")

    print("Security smoke tests passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"Security smoke test failed: {exc}", file=sys.stderr)
        raise
