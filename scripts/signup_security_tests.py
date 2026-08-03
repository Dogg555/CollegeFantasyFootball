#!/usr/bin/env python3
"""End-to-end signup reliability and security regression tests."""

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
PASSWORD = "Correct-Horse-Battery-Staple-2026!"


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
    headers: dict[str, str] | None = None,
) -> Response:
    final_headers = {
        "Accept": "application/json",
        "Origin": ALLOWED_ORIGIN,
        **(headers or {}),
    }
    body = None
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
        with urllib.request.urlopen(req, timeout=20) as response:
            return Response(
                response.status,
                {key.lower(): value for key, value in response.headers.items()},
                response.read(),
            )
    except urllib.error.HTTPError as error:
        return Response(
            error.code,
            {key.lower(): value for key, value in error.headers.items()},
            error.read(),
        )


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def wait_for_health() -> None:
    for _ in range(60):
        try:
            response = request("/health")
            if response.status == 200 and response.json().get("status") == "ok":
                return
        except Exception:
            pass
        time.sleep(2)
    raise AssertionError("API did not become healthy")


def signup(email: str, password: str = PASSWORD, request_id: str = "") -> Response:
    headers = {"Rndr-Id": request_id} if request_id else None
    return request(
        "/api/auth/signup",
        method="POST",
        payload={"email": email, "password": password},
        headers=headers,
    )


def login(email: str, password: str = PASSWORD) -> Response:
    return request(
        "/api/auth/login",
        method="POST",
        payload={"email": email, "password": password},
    )


def assert_generic_signup_response(response: Response) -> dict[str, Any]:
    payload = response.json()
    require(
        response.status == 202,
        f"verification signup must return 202, got {response.status}: {payload!r}",
    )
    require(payload.get("status") == "accepted", f"generic signup status missing: {payload!r}")
    require(payload.get("signupAccepted") is True, f"generic signup marker missing: {payload!r}")
    require(payload.get("emailVerificationRequired") is True, f"verification requirement missing: {payload!r}")
    require(payload.get("valid") is False, f"verification signup must not create a session: {payload!r}")
    for forbidden in ("token", "email", "emailSent", "accountMayExist"):
        require(forbidden not in payload, f"generic signup response leaked {forbidden}: {payload!r}")
    serialized = json.dumps(payload).lower()
    require("already exists" not in serialized, f"signup response disclosed account existence: {payload!r}")
    require("account created" not in serialized, f"signup response disclosed account creation: {payload!r}")
    return payload


def assert_indistinguishable(first: Response, second: Response, label: str) -> None:
    require(
        first.status == second.status,
        f"{label} status differs for known and unknown accounts: {first.status} != {second.status}",
    )
    require(
        first.json() == second.json(),
        f"{label} body differs for known and unknown accounts: {first.json()!r} != {second.json()!r}",
    )


def main() -> int:
    wait_for_health()

    # Invalid input must not consume the strict signup allowance. The previous
    # ordering counted these before validation and locked an address/client out.
    for attempt in range(6):
        invalid = signup("not-an-email", request_id=f"invalid-email-{attempt}")
        require(invalid.status == 400, f"invalid email attempt {attempt} returned {invalid.status}")
        require(invalid.json().get("code") == "invalid_email", "invalid email code missing")

    for attempt in range(6):
        weak = signup("weak-shape@example.test", "password123")
        require(weak.status == 400, f"weak password attempt {attempt} returned {weak.status}")
        require(weak.json().get("code") == "weak_password", "weak password code missing")

    missing_password = request(
        "/api/auth/signup",
        method="POST",
        payload={"email": "missing-password@example.test"},
    )
    require(missing_password.status == 400, "missing password should be rejected")
    require(missing_password.json().get("code") == "invalid_password", "missing password code missing")

    email = "signup-hardening@example.test"
    created = signup(email, request_id="signup-security-created")
    created_json = assert_generic_signup_response(created)
    require(
        created.headers.get("x-cff-request-id") == "signup-security-created",
        "trusted request ID was not returned",
    )
    require(
        "x-cff-request-id" in created.headers.get("access-control-expose-headers", "").lower(),
        "request reference header is not CORS-exposed",
    )

    # The generic response must still correspond to a persisted, unverified
    # account. Login proves the account exists without exposing that fact from
    # the signup endpoint itself.
    unverified_login = login(email)
    require(unverified_login.status == 403, "new unverified account did not persist")
    require(
        "verification" in json.dumps(unverified_login.json()).lower(),
        "unverified login did not require verification",
    )

    duplicate = signup(email, request_id="signup-security-duplicate")
    duplicate_json = assert_generic_signup_response(duplicate)
    require(duplicate_json == created_json, "new and duplicate signup bodies are distinguishable")
    require(
        duplicate.headers.get("x-cff-request-id") == "signup-security-duplicate",
        "duplicate response lost request correlation",
    )

    # The first creation plus four generic duplicate requests are allowed. The
    # next valid-shaped request for the same address must be throttled.
    for attempt in range(3):
        repeated = signup(email)
        assert_generic_signup_response(repeated)
    throttled = signup(email)
    require(throttled.status == 429, f"per-email signup throttle did not activate: {throttled.status}")
    require(throttled.json().get("code") == "rate_limited", "rate-limit code missing")
    require(int(throttled.headers.get("retry-after", "0")) > 0, "Retry-After header missing")

    # Recovery copy may safely say "if the account exists". The security
    # invariant is that known and unknown addresses receive the same status and
    # body, including when transactional email delivery is unavailable.
    unknown = f"unknown-{time.time_ns()}@example.test"
    known_resend = request(
        "/api/auth/resend-verification",
        method="POST",
        payload={"email": email},
    )
    unknown_resend = request(
        "/api/auth/resend-verification",
        method="POST",
        payload={"email": unknown},
    )
    assert_indistinguishable(known_resend, unknown_resend, "resend-verification response")

    known_reset = request(
        "/api/auth/request-password-reset",
        method="POST",
        payload={"email": email},
    )
    unknown_reset = request(
        "/api/auth/request-password-reset",
        method="POST",
        payload={"email": unknown},
    )
    assert_indistinguishable(known_reset, unknown_reset, "password-reset request response")

    blocked_origin = request(
        "/api/auth/signup",
        method="POST",
        payload={"email": f"blocked-{time.time_ns()}@example.test", "password": PASSWORD},
        headers={"Origin": "https://attacker.invalid"},
    )
    require(blocked_origin.status == 403, "disallowed signup origin was not rejected")
    require(blocked_origin.json().get("code") == "origin_not_allowed", "origin error code missing")

    print("Signup security tests passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"Signup security test failed: {exc}", file=sys.stderr)
        raise
