#!/usr/bin/env python3
"""Non-destructive acceptance checks for a Render staging deployment."""

import json
import os
import sys
import time
import urllib.error
import urllib.request


BASE_URL = os.environ.get("CFF_API_BASE_URL", "").strip().rstrip("/")
FRONTEND_URL = os.environ.get("CFF_FRONTEND_BASE_URL", "").strip().rstrip("/")
ACCOUNT_EMAIL = os.environ.get("CFF_SMOKE_ACCOUNT_EMAIL", "").strip().lower()
ACCOUNT_PASSWORD = os.environ.get("CFF_SMOKE_ACCOUNT_PASSWORD", "")
ADMIN_API_TOKEN = os.environ.get("CFF_ADMIN_API_TOKEN", "").strip()
VERIFICATION_EMAIL_BASE = os.environ.get("CFF_SMOKE_VERIFICATION_EMAIL_BASE", "").strip().lower()
PROBE_PASSWORD = os.environ.get("CFF_SMOKE_PASSWORD", "SmokeTest123!")


class SmokeFailure(RuntimeError):
    pass


def env_flag(name: str, default: bool = False) -> bool:
    value = os.environ.get(name)
    if value is None:
        return default
    return value.strip().lower() in {"1", "true", "yes", "on"}


TEST_EMAIL_VERIFICATION = env_flag("CFF_SMOKE_TEST_EMAIL_VERIFICATION")


def require(value, name):
    if not value:
        raise SmokeFailure(f"Missing required environment variable: {name}")


def request(method, path, body=None, token=None, expected=(200,), origin=None, return_headers=False):
    data = None
    headers = {"Accept": "application/json"}
    if body is not None:
        data = json.dumps(body).encode("utf-8")
        headers["Content-Type"] = "application/json"
    if token:
        headers["Authorization"] = f"Bearer {token}"
    if origin:
        headers["Origin"] = origin
    req = urllib.request.Request(f"{BASE_URL}{path}", data=data, headers=headers, method=method)
    response_headers = {}
    try:
        with urllib.request.urlopen(req, timeout=30) as response:
            text = response.read().decode("utf-8")
            status = response.getcode()
            response_headers = {key.lower(): value for key, value in response.headers.items()}
    except urllib.error.HTTPError as exc:
        text = exc.read().decode("utf-8")
        status = exc.code
        response_headers = {key.lower(): value for key, value in exc.headers.items()}
    except urllib.error.URLError as exc:
        raise SmokeFailure(f"{method} {path} could not connect to {BASE_URL}: {exc}") from exc
    if status not in expected:
        raise SmokeFailure(f"{method} {path} expected {expected}, got {status}: {text}")
    if not text:
        payload = {}
    else:
        try:
            payload = json.loads(text)
        except json.JSONDecodeError:
            payload = {"raw": text}
    return (payload, response_headers) if return_headers else payload


def assert_true(condition, message):
    if not condition:
        raise SmokeFailure(message)


def plus_alias(email, suffix):
    if "@" not in email:
        raise SmokeFailure("CFF_SMOKE_VERIFICATION_EMAIL_BASE must be a valid email address")
    local, domain = email.rsplit("@", 1)
    local = local.split("+", 1)[0]
    return f"{local}+cff-smoke-{suffix}@{domain}"


def main():
    require(BASE_URL, "CFF_API_BASE_URL")
    require(FRONTEND_URL, "CFF_FRONTEND_BASE_URL")
    require(ADMIN_API_TOKEN, "CFF_ADMIN_API_TOKEN")
    if TEST_EMAIL_VERIFICATION:
        require(ACCOUNT_EMAIL, "CFF_SMOKE_ACCOUNT_EMAIL")
        require(ACCOUNT_PASSWORD, "CFF_SMOKE_ACCOUNT_PASSWORD")
        require(VERIFICATION_EMAIL_BASE, "CFF_SMOKE_VERIFICATION_EMAIL_BASE")

    suffix = str(int(time.time()))

    health = request("GET", "/health")
    assert_true(health.get("status") == "ok", f"backend is not healthy: {health}")
    assert_true(health.get("database") == "ok", f"database is not healthy: {health}")

    api_health, cors_headers = request(
        "GET", "/api/health", origin=FRONTEND_URL, return_headers=True
    )
    assert_true(api_health.get("service") == "college-ff-api", f"unexpected API health payload: {api_health}")
    assert_true(
        cors_headers.get("access-control-allow-origin") == FRONTEND_URL,
        f"ALLOWED_ORIGINS does not exactly allow the staging frontend: {cors_headers}",
    )
    request("GET", "/api/health", origin="https://not-allowed.invalid", expected=(403,))
    request("GET", "/api/leagues", expected=(401,))

    if TEST_EMAIL_VERIFICATION:
        verification_email = plus_alias(VERIFICATION_EMAIL_BASE, suffix)
        signup = request(
            "POST",
            "/api/auth/signup",
            {"email": verification_email, "password": PROBE_PASSWORD},
            expected=(201,),
        )
        assert_true(not signup.get("token"), f"verification-required signup returned a token: {signup}")
        resend = request("POST", "/api/auth/resend-verification", {"email": verification_email})
        assert_true(resend.get("status") == "ok", f"verification resend was not accepted: {resend}")

        login = request("POST", "/api/auth/login", {"email": ACCOUNT_EMAIL, "password": ACCOUNT_PASSWORD})
        token = login.get("token")
        active_email = ACCOUNT_EMAIL
    else:
        active_email = f"render-smoke+{suffix}@example.com"
        signup = request(
            "POST",
            "/api/auth/signup",
            {"email": active_email, "password": PROBE_PASSWORD},
            expected=(201,),
        )
        token = signup.get("token")
        assert_true(token, f"email-deferred signup did not return a session token: {signup}")

    assert_true(token, "staging authentication did not return a token")
    validate = request("GET", "/api/auth/validate", token=token)
    assert_true(validate.get("valid") is True, f"staging token did not validate: {validate}")

    league = request(
        "POST",
        "/api/leagues",
        {
            "name": f"Render Smoke League {suffix}",
            "teams": 10,
            "scoring": "ppr",
            "draftType": "snake",
            "invitedEmails": [],
            "rosterRules": {"qb": 0, "rb": 0, "wr": 0, "te": 0, "flex": 0, "bench": 8},
        },
        token=token,
        expected=(201,),
    )
    league_id = league.get("id")
    assert_true(league_id, f"league creation did not return an ID: {league}")

    leagues = request("GET", "/api/leagues", token=token)
    assert_true(any(item.get("id") == league_id for item in leagues), "created league is missing from list")

    request("GET", "/api/admin/ingest/cfbd/status", token=token, expected=(403,))
    admin_status = request("GET", "/api/admin/ingest/cfbd/status", token=ADMIN_API_TOKEN)
    assert_true(isinstance(admin_status, dict), f"unexpected admin ingest status: {admin_status}")

    print(json.dumps({
        "status": "ok",
        "baseUrl": BASE_URL,
        "frontendUrl": FRONTEND_URL,
        "email": active_email,
        "leagueId": league_id,
        "emailVerificationTested": TEST_EMAIL_VERIFICATION,
        "corsExactOriginVerified": True,
        "corsUntrustedOriginBlocked": True,
        "adminAuthorizationChecked": True,
    }, indent=2))


if __name__ == "__main__":
    try:
        main()
    except SmokeFailure as exc:
        print(json.dumps({"status": "failed", "error": str(exc)}, indent=2), file=sys.stderr)
        raise SystemExit(1)
