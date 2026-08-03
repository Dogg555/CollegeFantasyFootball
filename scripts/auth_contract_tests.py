#!/usr/bin/env python3
"""Stable authentication endpoint contracts for isolated test environments."""

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
MODE = os.environ.get("CFF_AUTH_CONTRACT_MODE", "verification-disabled").strip().lower()
ALLOWED_ORIGIN = os.environ.get("CFF_CONTRACT_ORIGIN", "https://frontend.example.test")
EMAIL = os.environ.get("CFF_CONTRACT_EMAIL", f"auth-contract-{time.time_ns()}@example.test").strip().lower()
PASSWORD = os.environ.get("CFF_CONTRACT_PASSWORD", "Contract-Test-Password-2026!")
NEW_PASSWORD = os.environ.get("CFF_CONTRACT_NEW_PASSWORD", "Contract-Test-New-Password-2026!")

VALID_MODES = {"verification-disabled", "verification-required", "database-unavailable"}


class ContractFailure(RuntimeError):
    """Raised when the API no longer matches its documented auth contract."""


@dataclass(frozen=True)
class Response:
    status: int
    headers: dict[str, str]
    body: bytes

    def json(self) -> Any:
        if not self.body:
            return None
        try:
            return json.loads(self.body.decode("utf-8"))
        except json.JSONDecodeError as exc:
            raise ContractFailure(
                f"response {self.status} was not valid JSON: {self.body[:500]!r}"
            ) from exc


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ContractFailure(message)


def request(
    method: str,
    path: str,
    *,
    payload: Any | None = None,
    raw_body: bytes | None = None,
    content_type: str | None = None,
    token: str = "",
    origin: str | None = ALLOWED_ORIGIN,
    headers: dict[str, str] | None = None,
    timeout: int = 20,
) -> Response:
    final_headers = {"Accept": "application/json", **(headers or {})}
    if origin is not None:
        final_headers["Origin"] = origin
    if token:
        final_headers["Authorization"] = f"Bearer {token}"

    body = raw_body
    if payload is not None:
        require(raw_body is None, "payload and raw_body cannot both be supplied")
        body = json.dumps(payload).encode("utf-8")
        content_type = content_type or "application/json"
    if content_type:
        final_headers["Content-Type"] = content_type

    req = urllib.request.Request(
        f"{BASE_URL}{path}",
        data=body,
        headers=final_headers,
        method=method,
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as response:
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
    except urllib.error.URLError as error:
        raise ContractFailure(f"{method} {path} could not reach {BASE_URL}: {error}") from error


def expect_status(response: Response, expected: int, label: str) -> Any:
    payload = response.json()
    require(
        response.status == expected,
        f"{label} expected HTTP {expected}, got {response.status}: {payload!r}",
    )
    return payload


def wait_for_api() -> None:
    last_error = "no response"
    for _ in range(90):
        try:
            response = request("GET", "/api/auth/status", timeout=3)
            if response.status == 200:
                return
            last_error = f"HTTP {response.status}: {response.body[:300]!r}"
        except Exception as exc:
            last_error = str(exc)
        time.sleep(2)
    raise ContractFailure(f"API did not become reachable: {last_error}")


def assert_no_keys(payload: dict[str, Any], forbidden: tuple[str, ...], label: str) -> None:
    for key in forbidden:
        require(key not in payload, f"{label} unexpectedly exposed {key}: {payload!r}")


def test_database_unavailable() -> dict[str, Any]:
    health = request("GET", "/health")
    health_payload = expect_status(health, 503, "degraded health")
    require(health_payload.get("status") == "degraded", f"health was not degraded: {health_payload!r}")
    require(
        health_payload.get("database") in {"unavailable", "not_configured", "not_compiled"},
        f"health did not identify database failure: {health_payload!r}",
    )

    status = request("GET", "/api/auth/status")
    status_payload = expect_status(status, 200, "auth readiness")
    require(status_payload.get("ready") is False, f"unavailable auth reported ready: {status_payload!r}")
    require(status_payload.get("loginEnabled") is False, f"login remained enabled: {status_payload!r}")
    require(status_payload.get("signupEnabled") is False, f"signup remained enabled: {status_payload!r}")
    require(status_payload.get("status") == "degraded", f"auth status was not degraded: {status_payload!r}")

    login = request(
        "POST",
        "/api/auth/login",
        payload={"email": EMAIL, "password": PASSWORD},
    )
    login_payload = expect_status(login, 503, "login with unavailable database")
    assert_no_keys(login_payload, ("token",), "database failure login")

    signup = request(
        "POST",
        "/api/auth/signup",
        payload={"email": EMAIL, "password": PASSWORD},
    )
    signup_payload = expect_status(signup, 503, "signup with unavailable database")
    assert_no_keys(signup_payload, ("token",), "database failure signup")

    validate = request(
        "GET",
        "/api/auth/validate",
        token="unavailable-database-test-token",
    )
    validate_payload = expect_status(validate, 503, "validation with unavailable database")
    require(validate_payload.get("valid") is False, f"unavailable validation was not false: {validate_payload!r}")
    require(validate_payload.get("unavailable") is True, f"unavailable marker missing: {validate_payload!r}")

    return {
        "status": "ok",
        "mode": MODE,
        "databaseFailureReported": True,
        "loginDisabled": True,
        "signupDisabled": True,
    }


def test_healthy_auth() -> dict[str, Any]:
    verification_required = MODE == "verification-required"

    health_payload = expect_status(request("GET", "/health"), 200, "health")
    require(health_payload.get("status") == "ok", f"health was not ok: {health_payload!r}")
    require(health_payload.get("database") == "ok", f"database was not healthy: {health_payload!r}")
    require(health_payload.get("databaseConfigured") is True, f"database config missing: {health_payload!r}")

    api_health = expect_status(request("GET", "/api/health"), 200, "API-prefixed health")
    require(api_health.get("service") == "college-ff-api", f"unexpected service: {api_health!r}")

    readiness = expect_status(request("GET", "/api/auth/status"), 200, "auth readiness")
    require(readiness.get("ready") is True, f"auth was not ready: {readiness!r}")
    require(readiness.get("loginEnabled") is True, f"login was not enabled: {readiness!r}")
    require(readiness.get("signupEnabled") is True, f"signup was not enabled: {readiness!r}")
    require(readiness.get("emailDeliveryConfigured") is True, f"test SMTP was not configured: {readiness!r}")
    require(
        readiness.get("emailVerificationRequired") is verification_required,
        f"verification mode mismatch: {readiness!r}",
    )

    preflight = request(
        "OPTIONS",
        "/api/auth/login",
        headers={
            "Access-Control-Request-Method": "POST",
            "Access-Control-Request-Headers": "content-type, authorization",
        },
    )
    require(preflight.status == 204, f"CORS preflight expected 204, got {preflight.status}")
    require(
        preflight.headers.get("access-control-allow-origin") == ALLOWED_ORIGIN,
        f"CORS allow-origin mismatch: {preflight.headers!r}",
    )
    require("post" in preflight.headers.get("access-control-allow-methods", "").lower(), "POST not allowed by CORS")
    allow_headers = preflight.headers.get("access-control-allow-headers", "").lower()
    require("content-type" in allow_headers and "authorization" in allow_headers, "auth headers not allowed by CORS")

    unsupported = request(
        "POST",
        "/api/auth/login",
        raw_body=b'{"email":"content-type@example.test","password":"Contract-Test-Password-2026!"}',
        content_type="text/plain",
    )
    unsupported_payload = expect_status(unsupported, 415, "unsupported auth content type")
    require(unsupported_payload.get("code") == "unsupported_content_type", f"content-type code missing: {unsupported_payload!r}")

    oversized = request(
        "POST",
        "/api/auth/login",
        raw_body=(b'{"email":"oversized@example.test","password":"' + b"x" * 9000 + b'"}'),
        content_type="application/json",
    )
    oversized_payload = expect_status(oversized, 413, "oversized auth body")
    require(oversized_payload.get("code") == "request_too_large", f"body-size code missing: {oversized_payload!r}")

    invalid_email = request(
        "POST",
        "/api/auth/signup",
        payload={"email": "not-an-email", "password": PASSWORD},
    )
    invalid_email_payload = expect_status(invalid_email, 400, "invalid signup email")
    require(invalid_email_payload.get("code") == "invalid_email", f"invalid-email code missing: {invalid_email_payload!r}")

    weak_password = request(
        "POST",
        "/api/auth/signup",
        payload={"email": "weak-password@example.test", "password": "password123"},
    )
    weak_password_payload = expect_status(weak_password, 400, "weak signup password")
    require(weak_password_payload.get("code") == "weak_password", f"weak-password code missing: {weak_password_payload!r}")

    missing_password = request(
        "POST",
        "/api/auth/signup",
        payload={"email": "missing-password@example.test"},
    )
    missing_password_payload = expect_status(missing_password, 400, "missing signup password")
    require(missing_password_payload.get("code") == "invalid_password", f"missing-password code missing: {missing_password_payload!r}")

    missing_login_password = request(
        "POST",
        "/api/auth/login",
        payload={"email": EMAIL},
    )
    expect_status(missing_login_password, 400, "missing login password")

    canonical_input = f"  {EMAIL.upper()}  "
    created = request(
        "POST",
        "/api/auth/signup",
        payload={"email": canonical_input, "password": PASSWORD},
    )
    created_payload = created.json()

    session_token = ""
    if verification_required:
        require(created.status == 202, f"verification signup returned {created.status}: {created_payload!r}")
        require(created_payload.get("status") == "accepted", f"generic signup status missing: {created_payload!r}")
        require(created_payload.get("signupAccepted") is True, f"signup marker missing: {created_payload!r}")
        require(created_payload.get("valid") is False, f"verification signup created a session: {created_payload!r}")
        require(created_payload.get("emailVerificationRequired") is True, f"verification flag missing: {created_payload!r}")
        assert_no_keys(
            created_payload,
            ("token", "email", "emailSent", "emailVerificationToken", "accountMayExist"),
            "verification signup",
        )

        before_verify = request(
            "POST",
            "/api/auth/login",
            payload={"email": EMAIL, "password": PASSWORD},
        )
        before_verify_payload = expect_status(before_verify, 403, "unverified login")
        require("verification" in json.dumps(before_verify_payload).lower(), "unverified login did not explain verification")

        resend = request(
            "POST",
            "/api/auth/resend-verification",
            payload={"email": EMAIL},
        )
        resend_payload = expect_status(resend, 200, "verification resend")
        verification_token = resend_payload.get("emailVerificationToken")
        require(verification_token, f"test verification token missing: {resend_payload!r}")

        verified = request(
            "POST",
            "/api/auth/verify-email",
            payload={"token": verification_token},
        )
        verified_payload = expect_status(verified, 200, "email verification")
        require(verified_payload.get("emailVerified") is True, f"verification did not persist: {verified_payload!r}")
        require(verified_payload.get("email") == EMAIL, f"verified email was not canonical: {verified_payload!r}")

        login = request(
            "POST",
            "/api/auth/login",
            payload={"email": canonical_input, "password": PASSWORD},
        )
        login_payload = expect_status(login, 200, "verified login")
        session_token = login_payload.get("token", "")
        require(session_token, f"verified login did not return a token: {login_payload!r}")
    else:
        require(created.status == 201, f"signup returned {created.status}: {created_payload!r}")
        require(created_payload.get("email") == EMAIL, f"signup email was not canonical: {created_payload!r}")
        require(created_payload.get("valid") is True, f"signup session was not valid: {created_payload!r}")
        require(created_payload.get("emailVerificationRequired") is False, f"verification flag mismatch: {created_payload!r}")
        session_token = created_payload.get("token", "")
        require(session_token, f"signup did not return a token: {created_payload!r}")

    validated = request("GET", "/api/auth/validate", token=session_token)
    validated_payload = expect_status(validated, 200, "session validation")
    require(validated_payload.get("valid") is True, f"session was not valid: {validated_payload!r}")
    require(validated_payload.get("email") == EMAIL, f"session email mismatch: {validated_payload!r}")

    duplicate = request(
        "POST",
        "/api/auth/signup",
        payload={"email": EMAIL.upper(), "password": PASSWORD},
    )
    duplicate_payload = expect_status(duplicate, 202, "duplicate signup")
    require(duplicate_payload.get("valid") is False, f"duplicate signup created a session: {duplicate_payload!r}")
    assert_no_keys(duplicate_payload, ("token", "email"), "duplicate signup")
    if verification_required:
        require(duplicate_payload == created_payload, f"verification signup disclosed duplicate state: {duplicate_payload!r}")
    else:
        require(duplicate_payload.get("accountMayExist") is True, f"duplicate marker missing: {duplicate_payload!r}")
        require(duplicate_payload.get("emailVerificationRequired") is False, f"duplicate verification flag mismatch: {duplicate_payload!r}")

    wrong_password = request(
        "POST",
        "/api/auth/login",
        payload={"email": EMAIL, "password": NEW_PASSWORD},
    )
    wrong_password_payload = expect_status(wrong_password, 401, "incorrect password")
    assert_no_keys(wrong_password_payload, ("token",), "incorrect-password response")

    known_reset = request(
        "POST",
        "/api/auth/request-password-reset",
        payload={"email": EMAIL},
    )
    known_reset_payload = expect_status(known_reset, 200, "known-account reset request")
    reset_token = known_reset_payload.get("passwordResetToken")
    require(reset_token, f"test reset token missing: {known_reset_payload!r}")

    unknown_email = f"unknown-{time.time_ns()}@example.test"
    unknown_reset = request(
        "POST",
        "/api/auth/request-password-reset",
        payload={"email": unknown_email},
    )
    unknown_reset_payload = expect_status(unknown_reset, 200, "unknown-account reset request")
    require(
        unknown_reset_payload.get("message") == known_reset_payload.get("message"),
        f"reset messages disclosed account state: {known_reset_payload!r} != {unknown_reset_payload!r}",
    )
    assert_no_keys(unknown_reset_payload, ("passwordResetToken", "email"), "unknown reset request")

    bad_reset = request(
        "POST",
        "/api/auth/reset-password",
        payload={"token": "invalid-reset-token", "password": NEW_PASSWORD},
    )
    expect_status(bad_reset, 400, "invalid reset token")

    reset = request(
        "POST",
        "/api/auth/reset-password",
        payload={"token": reset_token, "password": NEW_PASSWORD},
    )
    reset_payload = expect_status(reset, 200, "password reset")
    require(reset_payload.get("status") == "ok", f"password reset status missing: {reset_payload!r}")
    require(reset_payload.get("email") == EMAIL, f"password reset email mismatch: {reset_payload!r}")

    revoked = request("GET", "/api/auth/validate", token=session_token)
    expect_status(revoked, 401, "session revoked by password reset")

    old_password = request(
        "POST",
        "/api/auth/login",
        payload={"email": EMAIL, "password": PASSWORD},
    )
    expect_status(old_password, 401, "old password after reset")

    relogin = request(
        "POST",
        "/api/auth/login",
        payload={"email": EMAIL, "password": NEW_PASSWORD},
    )
    relogin_payload = expect_status(relogin, 200, "new password login")
    new_token = relogin_payload.get("token", "")
    require(new_token, f"new password login did not return a token: {relogin_payload!r}")

    logout = request("POST", "/api/auth/logout", token=new_token)
    logout_payload = expect_status(logout, 200, "logout")
    require(logout_payload.get("status") == "ok", f"logout status missing: {logout_payload!r}")
    expect_status(request("GET", "/api/auth/validate", token=new_token), 401, "logged-out token validation")

    blocked = request(
        "POST",
        "/api/auth/signup",
        payload={"email": f"blocked-{time.time_ns()}@example.test", "password": PASSWORD},
        origin="https://attacker.invalid",
    )
    blocked_payload = expect_status(blocked, 403, "blocked-origin signup")
    require(blocked_payload.get("code") == "origin_not_allowed", f"origin error code missing: {blocked_payload!r}")

    return {
        "status": "ok",
        "mode": MODE,
        "email": EMAIL,
        "verificationRequired": verification_required,
        "signupContractChecked": True,
        "duplicateContractChecked": True,
        "verificationContractChecked": verification_required,
        "passwordResetContractChecked": True,
        "sessionRevocationChecked": True,
        "corsContractChecked": True,
        "requestLimitsChecked": True,
    }


def main() -> int:
    require(MODE in VALID_MODES, f"unsupported CFF_AUTH_CONTRACT_MODE={MODE!r}")
    wait_for_api()
    result = test_database_unavailable() if MODE == "database-unavailable" else test_healthy_auth()
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(json.dumps({"status": "failed", "mode": MODE, "error": str(exc)}, indent=2), file=sys.stderr)
        raise
