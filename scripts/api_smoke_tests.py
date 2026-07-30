#!/usr/bin/env python3
import json
import os
import time
import urllib.error
import urllib.request


BASE_URL = os.environ.get("CFF_API_BASE_URL", "http://127.0.0.1:8080").rstrip("/")
PASSWORD = os.environ.get("CFF_SMOKE_PASSWORD", "SmokeTest123!")
EMAIL_PREFIX = os.environ.get("CFF_SMOKE_EMAIL_PREFIX", "smoke")


class SmokeFailure(RuntimeError):
    pass


def request(method, path, body=None, token=None, expected=(200,)):
    data = None
    headers = {"Accept": "application/json"}
    if body is not None:
        data = json.dumps(body).encode("utf-8")
        headers["Content-Type"] = "application/json"
    if token:
        headers["Authorization"] = f"Bearer {token}"
    req = urllib.request.Request(f"{BASE_URL}{path}", data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=20) as resp:
            text = resp.read().decode("utf-8")
            status = resp.getcode()
    except urllib.error.HTTPError as exc:
        text = exc.read().decode("utf-8")
        status = exc.code
    if status not in expected:
        raise SmokeFailure(f"{method} {path} expected {expected}, got {status}: {text}")
    if not text:
        return {}
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        return {"raw": text}


def assert_true(condition, message):
    if not condition:
        raise SmokeFailure(message)


def main():
    suffix = str(int(time.time()))
    email = f"{EMAIL_PREFIX}+{suffix}@example.com"

    health = request("GET", "/health")
    assert_true(health.get("status") in {"ok", "degraded"}, f"unexpected health payload: {health}")

    api_health = request("GET", "/api/health")
    assert_true(api_health.get("service") == "college-ff-api", f"unexpected api health payload: {api_health}")

    request("GET", "/api/leagues", expected=(401,))

    signup = request("POST", "/api/auth/signup", {"email": email, "password": PASSWORD}, expected=(201,))
    token = signup.get("token")
    assert_true(token, f"signup did not return token: {signup}")

    validate = request("GET", "/api/auth/validate", token=token)
    assert_true(validate.get("valid") is True, f"token did not validate: {validate}")

    login = request("POST", "/api/auth/login", {"email": email, "password": PASSWORD})
    login_token = login.get("token")
    assert_true(login_token, f"login did not return token: {login}")

    request("POST", "/api/auth/logout", token=login_token)
    request("GET", "/api/auth/validate", token=login_token, expected=(401,))

    league_payload = {
        "name": f"Smoke League {suffix}",
        "teams": 10,
        "scoring": "ppr",
        "draftType": "snake",
        "invitedEmails": [f"smoke-manager-{suffix}@example.com"],
    }
    league = request("POST", "/api/leagues", league_payload, token=token, expected=(201,))
    league_id = league.get("id")
    assert_true(league_id, f"league create did not return id: {league}")

    leagues = request("GET", "/api/leagues", token=token)
    assert_true(any(item.get("id") == league_id for item in leagues), "created league missing from list")

    settings_update = dict(league)
    settings_update["notes"] = "smoke settings update"
    updated = request("PUT", f"/api/leagues/{league_id}", settings_update, token=token)
    assert_true(updated.get("notes") == "smoke settings update", f"league update failed: {updated}")

    members = request("GET", f"/api/leagues/{league_id}/members", token=token)
    assert_true(len(members) >= 1, f"members missing: {members}")

    draft = request("GET", f"/api/leagues/{league_id}/draft", token=token)
    assert_true("status" in draft, f"draft state missing status: {draft}")

    transactions = request("GET", f"/api/leagues/{league_id}/transactions", token=token)
    assert_true(isinstance(transactions, list), f"transactions response is not a list: {transactions}")

    reset = request("POST", "/api/auth/request-password-reset", {"email": email})
    reset_token = reset.get("passwordResetToken")
    if reset_token:
        new_password = f"{PASSWORD}Reset"
        request("POST", "/api/auth/reset-password", {"token": reset_token, "password": new_password})
        request("GET", "/api/auth/validate", token=token, expected=(401,))
        relogin = request("POST", "/api/auth/login", {"email": email, "password": new_password})
        assert_true(relogin.get("token"), f"password reset login failed: {relogin}")

    print(json.dumps({
        "status": "ok",
        "baseUrl": BASE_URL,
        "email": email,
        "leagueId": league_id,
    }, indent=2))


if __name__ == "__main__":
    try:
        main()
    except SmokeFailure as exc:
        print(json.dumps({"status": "failed", "error": str(exc)}, indent=2))
        raise SystemExit(1)
