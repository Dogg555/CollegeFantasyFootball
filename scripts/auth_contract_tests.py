#!/usr/bin/env python3
"""Authentication endpoint contracts for isolated CI environments."""
from __future__ import annotations

import json
import os
import re
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
EMAIL = os.getenv("CFF_CONTRACT_EMAIL", f"auth-contract-{time.time_ns()}@example.test").strip().lower()
PASSWORD = os.getenv("CFF_CONTRACT_PASSWORD", "Contract-Test-Password-2026!")
NEW_PASSWORD = os.getenv("CFF_CONTRACT_NEW_PASSWORD", "Contract-Test-New-Password-2026!")
ADMIN_TOKEN = os.getenv("CFF_TEST_ADMIN_TOKEN", "")
MAIL_LOG = Path(os.getenv("CFF_CONTRACT_MAIL_LOG", "/tmp/cff-test-mail.jsonl"))
VALID_MODES = {"verification-disabled", "verification-required", "database-unavailable"}
TOKEN_RE = re.compile(r"token-[0-9a-f]{64}")


class ContractFailure(RuntimeError):
    pass


@dataclass(frozen=True)
class Response:
    status: int
    headers: dict[str, str]
    body: bytes

    def json(self) -> Any:
        if not self.body:
            return None
        try:
            return json.loads(self.body.decode())
        except json.JSONDecodeError as exc:
            raise ContractFailure(f"HTTP {self.status} returned non-JSON: {self.body[:300]!r}") from exc


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ContractFailure(message)


def call(
    method: str,
    path: str,
    *,
    payload: Any | None = None,
    raw: bytes | None = None,
    content_type: str | None = None,
    token: str = "",
    origin: str | None = ORIGIN,
    headers: dict[str, str] | None = None,
    timeout: int = 20,
) -> Response:
    request_headers = {"Accept": "application/json", **(headers or {})}
    if origin is not None:
        request_headers["Origin"] = origin
    if token:
        request_headers["Authorization"] = f"Bearer {token}"
    body = raw
    if payload is not None:
        require(raw is None, "payload and raw cannot both be used")
        body = json.dumps(payload).encode()
        content_type = content_type or "application/json"
    if content_type:
        request_headers["Content-Type"] = content_type
    request = urllib.request.Request(BASE + path, data=body, headers=request_headers, method=method)
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return Response(response.status, {k.lower(): v for k, v in response.headers.items()}, response.read())
    except urllib.error.HTTPError as error:
        return Response(error.code, {k.lower(): v for k, v in error.headers.items()}, error.read())
    except urllib.error.URLError as error:
        raise ContractFailure(f"{method} {path} could not reach {BASE}: {error}") from error


def expect(response: Response, status: int, label: str) -> Any:
    body = response.json()
    require(response.status == status, f"{label}: expected {status}, got {response.status}: {body!r}")
    return body


def no_keys(body: dict[str, Any], keys: tuple[str, ...], label: str) -> None:
    for key in keys:
        require(key not in body, f"{label} exposed {key}: {body!r}")


def wait_for_api() -> None:
    last = "no response"
    for _ in range(90):
        try:
            response = call("GET", "/api/auth/status", timeout=3)
            if response.status == 200:
                return
            last = f"HTTP {response.status}"
        except Exception as exc:
            last = str(exc)
        time.sleep(2)
    raise ContractFailure(f"API did not become ready: {last}")


def mail_count() -> int:
    if not MAIL_LOG.exists():
        return 0
    return len([line for line in MAIL_LOG.read_text().splitlines() if line.strip()])


def wait_for_mail_token(subject: str, after: int) -> str:
    state = "mail log absent"
    for _ in range(30):
        if MAIL_LOG.exists():
            lines = [line for line in MAIL_LOG.read_text().splitlines() if line.strip()]
            state = f"{len(lines)} messages"
            for line in reversed(lines[after:]):
                try:
                    data = str(json.loads(line).get("data", ""))
                except json.JSONDecodeError:
                    continue
                if subject in data and EMAIL in data.lower():
                    match = TOKEN_RE.search(data)
                    if match:
                        return match.group(0)
        time.sleep(1)
    raise ContractFailure(f"SMTP message {subject!r} did not arrive ({state})")


def database_unavailable_contracts() -> dict[str, Any]:
    health = expect(call("GET", "/health"), 503, "degraded health")
    require(health.get("status") == "degraded", f"health not degraded: {health!r}")
    require(health.get("database") in {"unavailable", "not_configured", "not_compiled"}, f"DB failure absent: {health!r}")

    status = expect(call("GET", "/api/auth/status"), 200, "auth readiness")
    require(status.get("ready") is False, f"unavailable auth reported ready: {status!r}")
    require(status.get("loginEnabled") is False, f"login remained enabled: {status!r}")
    require(status.get("signupEnabled") is False, f"signup remained enabled: {status!r}")

    login = expect(call("POST", "/api/auth/login", payload={"email": EMAIL, "password": PASSWORD}), 503, "DB-down login")
    signup = expect(call("POST", "/api/auth/signup", payload={"email": EMAIL, "password": PASSWORD}), 503, "DB-down signup")
    validate = expect(call("GET", "/api/auth/validate", token="unavailable-test-token"), 503, "DB-down validate")
    no_keys(login, ("token",), "DB-down login")
    no_keys(signup, ("token",), "DB-down signup")
    require(validate.get("valid") is False and validate.get("unavailable") is True, f"validate outage markers absent: {validate!r}")
    return {"status": "ok", "mode": MODE, "databaseFailureReported": True}


def healthy_contracts() -> dict[str, Any]:
    verification = MODE == "verification-required"

    health = expect(call("GET", "/health"), 200, "health")
    require(health.get("status") == "ok" and health.get("database") == "ok", f"health not ready: {health!r}")
    api_health = expect(call("GET", "/api/health"), 200, "API health")
    require(api_health.get("service") == "college-ff-api", f"unexpected service: {api_health!r}")

    status = expect(call("GET", "/api/auth/status"), 200, "auth readiness")
    require(status.get("ready") is True, f"auth not ready: {status!r}")
    require(status.get("databaseConfigured") is True and status.get("database") == "ok", f"DB readiness wrong: {status!r}")
    require(status.get("loginEnabled") is True and status.get("signupEnabled") is True, f"auth disabled: {status!r}")
    require(status.get("emailDeliveryConfigured") is True, f"SMTP not configured: {status!r}")
    require(status.get("emailVerificationRequired") is verification, f"verification mode wrong: {status!r}")

    preflight = call("OPTIONS", "/api/auth/login", headers={
        "Access-Control-Request-Method": "POST",
        "Access-Control-Request-Headers": "content-type, authorization",
    })
    require(preflight.status == 204, f"preflight returned {preflight.status}")
    require(preflight.headers.get("access-control-allow-origin") == ORIGIN, f"CORS origin wrong: {preflight.headers!r}")
    require("post" in preflight.headers.get("access-control-allow-methods", "").lower(), "POST absent from CORS")
    allow_headers = preflight.headers.get("access-control-allow-headers", "").lower()
    require("content-type" in allow_headers and "authorization" in allow_headers, "auth headers absent from CORS")

    unsupported = expect(call("POST", "/api/auth/login", raw=b"{}", content_type="text/plain"), 415, "content type")
    require(unsupported.get("code") == "unsupported_content_type", f"wrong content-type code: {unsupported!r}")
    oversized = expect(call("POST", "/api/auth/login", raw=b"x" * 9000, content_type="application/json"), 413, "body limit")
    require(oversized.get("code") == "request_too_large", f"wrong body-limit code: {oversized!r}")

    invalid = expect(call("POST", "/api/auth/signup", payload={"email": "bad", "password": PASSWORD}), 400, "invalid email")
    weak = expect(call("POST", "/api/auth/signup", payload={"email": "weak@example.test", "password": "password123"}), 400, "weak password")
    missing = expect(call("POST", "/api/auth/signup", payload={"email": "missing@example.test"}), 400, "missing password")
    expect(call("POST", "/api/auth/login", payload={"email": EMAIL}), 400, "missing login password")
    require(invalid.get("code") == "invalid_email", f"invalid-email code wrong: {invalid!r}")
    require(weak.get("code") == "weak_password", f"weak-password code wrong: {weak!r}")
    require(missing.get("code") == "invalid_password", f"missing-password code wrong: {missing!r}")

    canonical = f"  {EMAIL.upper()}  "
    signup_response = call("POST", "/api/auth/signup", payload={"email": canonical, "password": PASSWORD})
    signup = signup_response.json()
    session = ""

    if verification:
        require(signup_response.status == 202, f"verification signup returned {signup_response.status}: {signup!r}")
        require(signup.get("status") == "accepted" and signup.get("signupAccepted") is True, f"generic signup absent: {signup!r}")
        require(signup.get("valid") is False and signup.get("emailVerificationRequired") is True, f"signup state wrong: {signup!r}")
        no_keys(signup, ("token", "email", "emailSent", "emailVerificationToken", "accountMayExist"), "verification signup")

        unverified = expect(call("POST", "/api/auth/login", payload={"email": EMAIL, "password": PASSWORD}), 403, "unverified login")
        require("verification" in json.dumps(unverified).lower(), f"verification explanation absent: {unverified!r}")

        before = mail_count()
        resend = expect(call("POST", "/api/auth/resend-verification", payload={"email": EMAIL}), 200, "resend")
        no_keys(resend, ("emailVerificationToken", "email"), "resend")
        verification_token = wait_for_mail_token("Verify your College Fantasy account", before)
        verified = expect(call("POST", "/api/auth/verify-email", payload={"token": verification_token}), 200, "verify email")
        require(verified.get("emailVerified") is True and verified.get("email") == EMAIL, f"verification not persisted: {verified!r}")

        login = expect(call("POST", "/api/auth/login", payload={"email": canonical, "password": PASSWORD}), 200, "verified login")
        session = str(login.get("token", ""))
    else:
        require(signup_response.status == 201, f"signup returned {signup_response.status}: {signup!r}")
        require(signup.get("email") == EMAIL and signup.get("valid") is True, f"signup state wrong: {signup!r}")
        require(signup.get("emailVerificationRequired") is False, f"verification flag wrong: {signup!r}")
        session = str(signup.get("token", ""))
    require(session, "signup/login did not return session token")

    validated = expect(call("GET", "/api/auth/validate", token=session), 200, "validate")
    require(validated.get("valid") is True and validated.get("email") == EMAIL, f"validation wrong: {validated!r}")

    ping_without_token = call("GET", "/api/secure/ping")
    require(
        ping_without_token.status == 401 and ping_without_token.body == b"unauthorized",
        f"secure ping unauthorized contract changed: {ping_without_token.status} {ping_without_token.body!r}",
    )
    ping = expect(call("GET", "/api/secure/ping", token=session), 200, "secure ping")
    require(
        ping == {"status": "ok", "scope": "secure"},
        f"secure ping payload changed: {ping!r}",
    )

    require(ADMIN_TOKEN, "operations contract token was not provided")
    admin_missing = expect(call("GET", "/api/admin/ingest/cfbd/status"), 401, "admin status without token")
    require(admin_missing.get("error") == "Unauthorized", f"admin missing-token response changed: {admin_missing!r}")
    admin_forbidden = expect(
        call("GET", "/api/admin/ingest/cfbd/status", token=session),
        403,
        "admin status with account token",
    )
    require(
        admin_forbidden.get("error") == "Admin access required",
        f"admin account guard changed: {admin_forbidden!r}",
    )
    expect(
        call("POST", "/api/admin/ingest/cfbd", token=session),
        403,
        "manual roster ingest guard",
    )
    expect(
        call("POST", "/api/admin/ingest/cfbd/live", token=session),
        403,
        "manual live ingest guard",
    )

    ingestion_status = expect(
        call("GET", "/api/admin/ingest/cfbd/status", token=ADMIN_TOKEN),
        200,
        "administrator ingestion status",
    )
    require(ingestion_status.get("configured") is True, f"ingestion database state wrong: {ingestion_status!r}")
    require(ingestion_status.get("status") == "ok", f"ingestion status wrong: {ingestion_status!r}")
    require(ingestion_status.get("fullRosterSchedule") == "weekly", f"roster schedule changed: {ingestion_status!r}")
    require(ingestion_status.get("manualTriggerAvailable") is True, f"manual trigger flag changed: {ingestion_status!r}")
    require(isinstance(ingestion_status.get("counts"), dict), f"ingestion counts missing: {ingestion_status!r}")
    require(isinstance(ingestion_status.get("runs"), list), f"ingestion runs missing: {ingestion_status!r}")

    live_status = expect(
        call("GET", "/api/admin/ingest/cfbd/live/status", token=ADMIN_TOKEN),
        200,
        "administrator live-ingest status",
    )
    require(isinstance(live_status, dict), f"live-ingest status is not an object: {live_status!r}")

    for operations_path in (
        "/api/secure/ping",
        "/api/admin/ingest/cfbd",
        "/api/admin/ingest/cfbd/status",
        "/api/admin/ingest/cfbd/live",
        "/api/admin/ingest/cfbd/live/status",
    ):
        operations_preflight = call(
            "OPTIONS",
            operations_path,
            headers={
                "Access-Control-Request-Method": "POST",
                "Access-Control-Request-Headers": "content-type, authorization",
            },
        )
        require(
            operations_preflight.status == 204,
            f"operations preflight failed for {operations_path}: {operations_preflight.status}",
        )
        require(
            operations_preflight.headers.get("access-control-allow-origin") == ORIGIN,
            f"operations CORS origin wrong for {operations_path}: {operations_preflight.headers!r}",
        )

    duplicate = expect(call("POST", "/api/auth/signup", payload={"email": EMAIL.upper(), "password": PASSWORD}), 202, "duplicate signup")
    require(duplicate.get("valid") is False, f"duplicate created session: {duplicate!r}")
    no_keys(duplicate, ("token", "email"), "duplicate")
    if verification:
        require(duplicate == signup, f"duplicate disclosed state: {duplicate!r}")
    else:
        require(duplicate.get("accountMayExist") is True, f"duplicate marker absent: {duplicate!r}")

    wrong = expect(call("POST", "/api/auth/login", payload={"email": EMAIL, "password": NEW_PASSWORD}), 401, "wrong password")
    no_keys(wrong, ("token",), "wrong password")

    before = mail_count()
    known_reset = expect(call("POST", "/api/auth/request-password-reset", payload={"email": EMAIL}), 200, "known reset")
    no_keys(known_reset, ("passwordResetToken", "email"), "known reset")
    reset_token = wait_for_mail_token("Reset your College Fantasy password", before)

    unknown_reset = expect(call("POST", "/api/auth/request-password-reset", payload={"email": f"unknown-{time.time_ns()}@example.test"}), 200, "unknown reset")
    require(unknown_reset.get("message") == known_reset.get("message"), "reset response disclosed account existence")
    no_keys(unknown_reset, ("passwordResetToken", "email"), "unknown reset")

    expect(call("POST", "/api/auth/reset-password", payload={"token": "invalid", "password": NEW_PASSWORD}), 400, "bad reset token")
    reset = expect(call("POST", "/api/auth/reset-password", payload={"token": reset_token, "password": NEW_PASSWORD}), 200, "reset password")
    require(reset.get("status") == "ok" and reset.get("email") == EMAIL, f"reset result wrong: {reset!r}")
    expect(call("GET", "/api/auth/validate", token=session), 401, "reset revocation")
    expect(call("POST", "/api/auth/login", payload={"email": EMAIL, "password": PASSWORD}), 401, "old password")

    relogin = expect(call("POST", "/api/auth/login", payload={"email": EMAIL, "password": NEW_PASSWORD}), 200, "new password login")
    new_session = str(relogin.get("token", ""))
    require(new_session, "new password login did not return token")
    logout = expect(call("POST", "/api/auth/logout", token=new_session), 200, "logout")
    require(logout.get("status") == "ok", f"logout wrong: {logout!r}")
    expect(call("GET", "/api/auth/validate", token=new_session), 401, "logout revocation")

    blocked = expect(call("POST", "/api/auth/signup", payload={"email": f"blocked-{time.time_ns()}@example.test", "password": PASSWORD}, origin="https://attacker.invalid"), 403, "blocked origin")
    require(blocked.get("code") == "origin_not_allowed", f"origin code wrong: {blocked!r}")

    return {
        "status": "ok",
        "mode": MODE,
        "verificationRequired": verification,
        "signup": True,
        "login": True,
        "duplicateProtection": True,
        "verification": verification,
        "passwordReset": True,
        "sessionRevocation": True,
        "cors": True,
        "requestLimits": True,
        "operationsRoutes": True,
        "operationsAuthorization": True,
        "operationsCors": True,
    }


def main() -> int:
    require(MODE in VALID_MODES, f"unsupported mode: {MODE}")
    wait_for_api()
    result = database_unavailable_contracts() if MODE == "database-unavailable" else healthy_contracts()
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(json.dumps({"status": "failed", "mode": MODE, "error": str(exc)}, indent=2), file=sys.stderr)
        raise
