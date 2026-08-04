#!/usr/bin/env python3
"""Production password-reset expiration and single-use contracts."""
from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Any

BASE = os.getenv("CFF_API_BASE_URL", "http://127.0.0.1:8080").rstrip("/")
MODE = os.getenv("CFF_AUTH_CONTRACT_MODE", "verification-disabled").strip().lower()
ORIGIN = os.getenv("CFF_CONTRACT_ORIGIN", "https://frontend.example.test")
EMAIL = os.getenv("CFF_CONTRACT_EMAIL", f"password-reset-{time.time_ns()}@example.test").strip().lower()
PASSWORD = os.getenv("CFF_CONTRACT_PASSWORD", "Password-Reset-Contract-2026!")
NEW_PASSWORD = os.getenv("CFF_CONTRACT_NEW_PASSWORD", "Password-Reset-New-Contract-2026!")
DB_URL = os.getenv("CFF_CONTRACT_DB_URL", "").strip()
MAIL_LOG = Path(os.getenv("CFF_CONTRACT_MAIL_LOG", "/tmp/cff-test-mail.jsonl"))
TOKEN_RE = re.compile(r"token-[0-9a-f]{64}")
VALID_MODES = {"verification-disabled", "verification-required"}


class ContractFailure(RuntimeError):
    pass


@dataclass(frozen=True)
class Response:
    status: int
    body: bytes

    def json(self) -> Any:
        if not self.body:
            return None
        try:
            return json.loads(self.body.decode())
        except json.JSONDecodeError as exc:
            raise ContractFailure(
                f"HTTP {self.status} returned non-JSON: {self.body[:300]!r}"
            ) from exc


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ContractFailure(message)


def call(
    method: str,
    path: str,
    *,
    payload: Any | None = None,
    token: str = "",
    timeout: int = 20,
) -> Response:
    headers = {"Accept": "application/json", "Origin": ORIGIN}
    body = None
    if payload is not None:
        body = json.dumps(payload).encode()
        headers["Content-Type"] = "application/json"
    if token:
        headers["Authorization"] = f"Bearer {token}"
    request = urllib.request.Request(BASE + path, data=body, headers=headers, method=method)
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return Response(response.status, response.read())
    except urllib.error.HTTPError as error:
        return Response(error.code, error.read())
    except urllib.error.URLError as error:
        raise ContractFailure(f"{method} {path} could not reach {BASE}: {error}") from error


def expect(response: Response, status: int, label: str) -> Any:
    body = response.json()
    require(
        response.status == status,
        f"{label}: expected {status}, got {response.status}: {body!r}",
    )
    return body


def wait_for_api() -> None:
    last = "no response"
    for _ in range(90):
        try:
            response = call("GET", "/api/auth/status", timeout=3)
            if response.status == 200:
                return
            last = f"HTTP {response.status}"
        except Exception as exc:  # pragma: no cover - diagnostic path
            last = str(exc)
        time.sleep(2)
    raise ContractFailure(f"API did not become ready: {last}")


def mail_count() -> int:
    if not MAIL_LOG.exists():
        return 0
    return len([line for line in MAIL_LOG.read_text(encoding="utf-8").splitlines() if line.strip()])


def wait_for_mail_token(subject: str, email: str, after: int) -> str:
    state = "mail log absent"
    for _ in range(30):
        if MAIL_LOG.exists():
            lines = [
                line
                for line in MAIL_LOG.read_text(encoding="utf-8").splitlines()
                if line.strip()
            ]
            state = f"{len(lines)} messages"
            for line in reversed(lines[after:]):
                try:
                    data = str(json.loads(line).get("data", ""))
                except json.JSONDecodeError:
                    continue
                if subject in data and email in data.lower():
                    match = TOKEN_RE.search(data)
                    if match:
                        return match.group(0)
        time.sleep(1)
    raise ContractFailure(f"SMTP message {subject!r} did not arrive ({state})")


def sql_literal(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def database_scalar(sql: str) -> str:
    require(DB_URL, "CFF_CONTRACT_DB_URL is required for expiration contracts")
    command = [
        "docker",
        "run",
        "--rm",
        "--network",
        "host",
        "postgres:16",
        "psql",
        DB_URL,
        "-v",
        "ON_ERROR_STOP=1",
        "-tA",
        "-c",
        sql,
    ]
    result = subprocess.run(command, check=False, capture_output=True, text=True, timeout=60)
    if result.returncode != 0:
        raise ContractFailure(
            f"database contract command failed ({result.returncode}): {result.stderr.strip()}"
        )
    return result.stdout.strip()


def request_reset_token() -> str:
    before = mail_count()
    response = expect(
        call("POST", "/api/auth/request-password-reset", payload={"email": EMAIL}),
        200,
        "request password reset",
    )
    require(
        response.get("message") == "If the account exists, a password reset email will be sent.",
        f"password-reset request disclosed account state: {response!r}",
    )
    require("passwordResetToken" not in response and "email" not in response,
            f"password-reset request exposed recovery data: {response!r}")
    return wait_for_mail_token("Reset your College Fantasy password", EMAIL, before)


def main() -> int:
    require(MODE in VALID_MODES, f"unsupported mode: {MODE}")
    wait_for_api()

    signup_mail_before = mail_count()
    signup_response = call(
        "POST",
        "/api/auth/signup",
        payload={"email": EMAIL, "password": PASSWORD},
    )
    expected_signup = 202 if MODE == "verification-required" else 201
    signup = expect(signup_response, expected_signup, "signup")
    require("passwordResetToken" not in signup, f"signup exposed reset data: {signup!r}")

    if MODE == "verification-required":
        verification_token = wait_for_mail_token(
            "Verify your College Fantasy account", EMAIL, signup_mail_before
        )
        verified = expect(
            call("POST", "/api/auth/verify-email", payload={"token": verification_token}),
            200,
            "verify account",
        )
        require(verified.get("emailVerified") is True, f"verification failed: {verified!r}")

    login = expect(
        call("POST", "/api/auth/login", payload={"email": EMAIL, "password": PASSWORD}),
        200,
        "initial login",
    )
    first_session = str(login.get("token", ""))
    require(first_session, "initial login did not return a bearer token")

    expired_token = request_reset_token()
    expiry_state = database_scalar(
        "SELECT (password_reset_token IS NOT NULL)::int || '|' || "
        "(password_reset_expires_at > NOW())::int FROM users WHERE email = "
        + sql_literal(EMAIL)
    )
    require(expiry_state == "1|1", f"new reset token was not stored with a future expiry: {expiry_state!r}")

    updated = database_scalar(
        "UPDATE users SET password_reset_expires_at = NOW() - INTERVAL '1 second' "
        "WHERE email = " + sql_literal(EMAIL) + " RETURNING 1"
    )
    require(updated == "1", "test could not force the reset token to expire")

    expired = expect(
        call(
            "POST",
            "/api/auth/reset-password",
            payload={"token": expired_token, "password": NEW_PASSWORD},
        ),
        400,
        "expired reset token",
    )
    require(
        "invalid or expired" in str(expired.get("error", "")).lower(),
        f"expired-token response was not actionable: {expired!r}",
    )
    expect(
        call("GET", "/api/auth/validate", token=first_session),
        200,
        "expired token must not revoke sessions",
    )
    expect(
        call("POST", "/api/auth/login", payload={"email": EMAIL, "password": PASSWORD}),
        200,
        "expired token must not change password",
    )

    usable_token = request_reset_token()
    require(usable_token != expired_token, "a new reset request reused the expired raw token")
    reset = expect(
        call(
            "POST",
            "/api/auth/reset-password",
            payload={"token": usable_token, "password": NEW_PASSWORD},
        ),
        200,
        "valid reset token",
    )
    require(
        reset.get("status") == "ok" and reset.get("email") == EMAIL,
        f"valid reset did not complete: {reset!r}",
    )

    token_state = database_scalar(
        "SELECT (password_reset_token IS NULL)::int || '|' || "
        "(password_reset_expires_at IS NULL)::int || '|' || "
        "(SELECT COUNT(*) FROM auth_tokens WHERE email = " + sql_literal(EMAIL) + ") "
        "FROM users WHERE email = " + sql_literal(EMAIL)
    )
    require(token_state == "1|1|0", f"reset token or prior sessions survived consumption: {token_state!r}")

    expect(
        call("GET", "/api/auth/validate", token=first_session),
        401,
        "password reset must revoke prior sessions",
    )
    expect(
        call(
            "POST",
            "/api/auth/reset-password",
            payload={"token": usable_token, "password": PASSWORD},
        ),
        400,
        "consumed reset token reuse",
    )
    expect(
        call("POST", "/api/auth/login", payload={"email": EMAIL, "password": PASSWORD}),
        401,
        "old password after reset",
    )
    new_login = expect(
        call("POST", "/api/auth/login", payload={"email": EMAIL, "password": NEW_PASSWORD}),
        200,
        "new password after reset",
    )
    require(new_login.get("token"), "new password login did not return a bearer token")

    print(json.dumps({
        "status": "ok",
        "mode": MODE,
        "expiredTokenRejected": True,
        "expiredTokenSideEffectFree": True,
        "singleUseToken": True,
        "sessionsRevoked": True,
        "passwordChanged": True,
    }, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(json.dumps({"status": "failed", "mode": MODE, "error": str(exc)}, indent=2), file=sys.stderr)
        raise
