#!/usr/bin/env python3
"""End-to-end API smoke checks for local Docker and deployed environments."""

import json
import os
import sys
import time
import urllib.error
import urllib.request


BASE_URL = os.environ.get("CFF_API_BASE_URL", "http://127.0.0.1:8080").strip().rstrip("/")
DEFAULT_PASSWORD = os.environ.get("CFF_SMOKE_PASSWORD", "SmokeTest123!")
EMAIL_PREFIX = os.environ.get("CFF_SMOKE_EMAIL_PREFIX", "smoke")
ACCOUNT_EMAIL = os.environ.get("CFF_SMOKE_ACCOUNT_EMAIL", "").strip().lower()
ACCOUNT_PASSWORD = os.environ.get("CFF_SMOKE_ACCOUNT_PASSWORD", DEFAULT_PASSWORD)
ADMIN_API_TOKEN = os.environ.get("CFF_ADMIN_API_TOKEN", "").strip()
VERIFICATION_EMAIL_BASE = os.environ.get("CFF_SMOKE_VERIFICATION_EMAIL_BASE", "").strip().lower()


class SmokeFailure(RuntimeError):
    """Raised when a smoke-test assertion fails."""


def env_flag(name: str, default: bool = False) -> bool:
    value = os.environ.get(name)
    if value is None:
        return default
    return value.strip().lower() in {"1", "true", "yes", "on"}


def request(method, path, body=None, token=None, expected=(200,)):
    data = None
    headers = {"Accept": "application/json"}
    if body is not None:
        data = json.dumps(body).encode("utf-8")
        headers["Content-Type"] = "application/json"
    if token:
        headers["Authorization"] = f"Bearer {token}"

    req = urllib.request.Request(
        f"{BASE_URL}{path}",
        data=data,
        headers=headers,
        method=method,
    )
    try:
        with urllib.request.urlopen(req, timeout=30) as response:
            text = response.read().decode("utf-8")
            status = response.getcode()
    except urllib.error.HTTPError as exc:
        text = exc.read().decode("utf-8")
        status = exc.code
    except urllib.error.URLError as exc:
        raise SmokeFailure(f"{method} {path} could not connect to {BASE_URL}: {exc}") from exc

    if status not in expected:
        raise SmokeFailure(f"{method} {path} expected {expected}, got {status}: {text}")
    if not text:
        return {}
    try:
        return json.loads(text)
    except json.JSONDecodeError as exc:
        raise SmokeFailure(f"{method} {path} returned invalid JSON: {text}") from exc


def assert_true(condition, message):
    if not condition:
        raise SmokeFailure(message)


def plus_alias(email: str, suffix: str) -> str:
    if "@" not in email:
        raise SmokeFailure("CFF_SMOKE_VERIFICATION_EMAIL_BASE must be a valid email address")
    local, domain = email.rsplit("@", 1)
    local = local.split("+", 1)[0]
    return f"{local}+cff-smoke-{suffix}@{domain}"


def main():
    suffix = str(int(time.time()))
    verification_email = None

    health = request("GET", "/health")
    assert_true(health.get("status") == "ok", f"backend is not healthy: {health}")
    assert_true(health.get("database") == "ok", f"database is not healthy: {health}")

    api_health = request("GET", "/api/health")
    assert_true(api_health.get("service") == "college-ff-api", f"unexpected API health payload: {api_health}")
    request("GET", "/api/leagues", expected=(401,))

    created_account = not ACCOUNT_EMAIL
    if created_account:
        email = f"{EMAIL_PREFIX}+{suffix}@example.com"
        password = DEFAULT_PASSWORD
        signup = request(
            "POST",
            "/api/auth/signup",
            {"email": email, "password": password},
            expected=(201,),
        )
        token = signup.get("token")
        assert_true(
            token,
            "signup did not return a token. For verification-required deployments, configure "
            "CFF_SMOKE_ACCOUNT_EMAIL and CFF_SMOKE_ACCOUNT_PASSWORD with a preverified account.",
        )

        second_login = request("POST", "/api/auth/login", {"email": email, "password": password})
        second_token = second_login.get("token")
        assert_true(second_token, f"login did not return token: {second_login}")
        request("POST", "/api/auth/logout", token=second_token)
        request("GET", "/api/auth/validate", token=second_token, expected=(401,))
    else:
        email = ACCOUNT_EMAIL
        password = ACCOUNT_PASSWORD
        login = request("POST", "/api/auth/login", {"email": email, "password": password})
        token = login.get("token")
        assert_true(token, f"configured smoke account login did not return token: {login}")

        if env_flag("CFF_SMOKE_TEST_EMAIL_VERIFICATION"):
            assert_true(
                VERIFICATION_EMAIL_BASE,
                "CFF_SMOKE_VERIFICATION_EMAIL_BASE is required when "
                "CFF_SMOKE_TEST_EMAIL_VERIFICATION=true",
            )
            verification_email = plus_alias(VERIFICATION_EMAIL_BASE, suffix)
            verification_signup = request(
                "POST",
                "/api/auth/signup",
                {"email": verification_email, "password": DEFAULT_PASSWORD},
                expected=(201,),
            )
            assert_true(
                not verification_signup.get("token"),
                f"verification-required signup unexpectedly returned a token: {verification_signup}",
            )
            resend = request(
                "POST",
                "/api/auth/resend-verification",
                {"email": verification_email},
            )
            assert_true(resend.get("status") == "ok", f"verification resend was not accepted: {resend}")

    validate = request("GET", "/api/auth/validate", token=token)
    assert_true(validate.get("valid") is True, f"token did not validate: {validate}")

    league_payload = {
        "name": f"Smoke League {suffix}",
        "teams": 10,
        "scoring": "ppr",
        "draftType": "snake",
        "invitedEmails": [],
        "rosterRules": {"qb": 0, "rb": 0, "wr": 0, "te": 0, "flex": 0, "bench": 8},
    }
    league = request("POST", "/api/leagues", league_payload, token=token, expected=(201,))
    league_id = league.get("id")
    assert_true(league_id, f"league creation did not return an ID: {league}")

    leagues = request("GET", "/api/leagues", token=token)
    assert_true(any(item.get("id") == league_id for item in leagues), "created league is missing from list")

    settings_update = dict(league)
    settings_update["notes"] = "smoke settings update"
    updated = request("PUT", f"/api/leagues/{league_id}", settings_update, token=token)
    assert_true(updated.get("notes") == "smoke settings update", f"league update failed: {updated}")

    members = request("GET", f"/api/leagues/{league_id}/members", token=token)
    assert_true(isinstance(members, list) and len(members) >= 1, f"league members missing: {members}")

    draft = request("GET", f"/api/leagues/{league_id}/draft", token=token)
    assert_true("status" in draft, f"draft state is missing status: {draft}")

    request("GET", "/api/admin/ingest/cfbd/status", token=token, expected=(403,))
    if ADMIN_API_TOKEN:
        admin_status = request("GET", "/api/admin/ingest/cfbd/status", token=ADMIN_API_TOKEN)
        assert_true(isinstance(admin_status, dict), f"unexpected admin ingestion status: {admin_status}")

    transactions = request("GET", f"/api/leagues/{league_id}/transactions", token=token)
    assert_true(isinstance(transactions, list), f"transactions response is not a list: {transactions}")

    if created_account:
        reset = request("POST", "/api/auth/request-password-reset", {"email": email})
        reset_token = reset.get("passwordResetToken")
        if reset_token:
            new_password = f"{password}Reset"
            request("POST", "/api/auth/reset-password", {"token": reset_token, "password": new_password})
            request("GET", "/api/auth/validate", token=token, expected=(401,))
            relogin = request("POST", "/api/auth/login", {"email": email, "password": new_password})
            assert_true(relogin.get("token"), f"password-reset login failed: {relogin}")

    print(
        json.dumps(
            {
                "status": "ok",
                "baseUrl": BASE_URL,
                "email": email,
                "leagueId": league_id,
                "verificationEmailPending": verification_email,
                "adminAuthorizationChecked": bool(ADMIN_API_TOKEN),
            },
            indent=2,
        )
    )


if __name__ == "__main__":
    try:
        main()
    except SmokeFailure as exc:
        print(json.dumps({"status": "failed", "error": str(exc)}, indent=2), file=sys.stderr)
        raise SystemExit(1)
