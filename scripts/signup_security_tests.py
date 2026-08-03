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

    timestamp = time.time_ns()
    email = f"signup-hardening-{timestamp}@example.test"
    created = signup(email, request_id="signup-security-created")
    require(created.status == 201, f"new account signup returned {created.status}: {created.body!r}")
    created_json = created.json()
    require(created_json.get("email") == email, "signup did not canonicalize or return the account email")
    require(created_json.get("emailVerificationRequired") is True, "verification must be required in this test")
    require(created_json.get("valid") is False, "unverified signup must not create a valid session")
    require("token" not in created_json, "unverified signup exposed a session token")
    require(created_json.get("emailSent") is False, "test environment should report unavailable email delivery")
    require(
        created.headers.get("x-cff-request-id") == "signup-security-created",
        "trusted request ID was not returned",
    )
    require(
        "x-cff-request-id" in created.headers.get("access-control-expose-headers", "").lower(),
        "request reference header is not CORS-exposed",
    )

    duplicate = signup(email, request_id="signup-security-duplicate")
    require(duplicate.status == 202, f"duplicate signup should be accepted generically, got {duplicate.status}")
    duplicate_json = duplicate.json()
    require(duplicate_json.get("accountMayExist") is True, "generic duplicate marker missing")
    require(duplicate_json.get("valid") is False, "generic duplicate response must not create a session")
    require("token" not in duplicate_json, "generic duplicate response exposed a token")
    serialized_duplicate = json.dumps(duplicate_json).lower()
    require("already exists" not in serialized_duplicate, "duplicate signup disclosed account existence")
    require(email not in serialized_duplicate, "duplicate signup reflected the registered email")
    require(
        duplicate.headers.get("x-cff-request-id") == "signup-security-duplicate",
        "duplicate response lost request correlation",
    )

    # The first creation plus four generic duplicate requests are allowed. The
    # next valid-shaped request for the same address must be throttled.
    for attempt in range(3):
        repeated = signup(email)
        require(repeated.status == 202, f"duplicate attempt {attempt} returned {repeated.status}")
    throttled = signup(email)
    require(throttled.status == 429, f"per-email signup throttle did not activate: {throttled.status}")
    require(throttled.json().get("code") == "rate_limited", "rate-limit code missing")
    require(int(throttled.headers.get("retry-after", "0")) > 0, "Retry-After header missing")

    # Recovery endpoints must remain enumeration-safe for unknown accounts.
    unknown = f"unknown-{timestamp}@example.test"
    resend = request(
        "/api/auth/resend-verification",
        method="POST",
        payload={"email": unknown},
    )
    require(resend.status == 200, "unknown-account resend must return a generic success")
    require("exists" not in json.dumps(resend.json()).lower(), "resend response disclosed account existence")

    reset = request(
        "/api/auth/request-password-reset",
        method="POST",
        payload={"email": unknown},
    )
    require(reset.status == 200, "unknown-account reset request must return a generic success")
    require("exists" not in json.dumps(reset.json()).lower(), "reset response disclosed account existence")

    blocked_origin = request(
        "/api/auth/signup",
        method="POST",
        payload={"email": f"blocked-{timestamp}@example.test", "password": PASSWORD},
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
