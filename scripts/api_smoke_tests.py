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
    manager_email = f"{EMAIL_PREFIX}-manager+{suffix}@example.com"

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
        "invitedEmails": [manager_email],
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

    manager_signup = request("POST", "/api/auth/signup", {"email": manager_email, "password": PASSWORD}, expected=(201,))
    manager_token = manager_signup.get("token")
    assert_true(manager_token, f"manager signup did not return token: {manager_signup}")
    joined = request("POST", f"/api/leagues/{league_id}/join", token=manager_token)
    assert_true(joined.get("id") == league_id, f"manager could not join invited league: {joined}")

    members = request("GET", f"/api/leagues/{league_id}/members", token=token)
    member_emails = {member.get("email") for member in members}
    assert_true({email, manager_email}.issubset(member_emails), f"joined members missing: {members}")

    draft = request("GET", f"/api/leagues/{league_id}/draft", token=token)
    assert_true("status" in draft, f"draft state missing status: {draft}")

    draft_order = [email, manager_email]
    order_state = request(
        "PUT",
        f"/api/leagues/{league_id}/draft/order",
        {"draftOrder": draft_order},
        token=token,
    )
    assert_true(order_state.get("draftOrder") == draft_order, f"draft order was not saved: {order_state}")

    smoke_player_1 = {
        "id": f"smoke-player-{suffix}",
        "name": "Smoke Test RB",
        "team": "Test State",
        "position": "RB",
        "conference": "Smoke",
        "projection": 18.4,
        "rank": 1,
    }
    pick_state = request(
        "POST",
        f"/api/leagues/{league_id}/draft/picks",
        {"player": smoke_player_1},
        token=token,
        expected=(201,),
    )
    assert_true(len(pick_state.get("picks", [])) == 1, f"draft pick was not recorded: {pick_state}")
    assert_true(pick_state.get("currentPick") == 2, f"draft pick did not advance current pick: {pick_state}")
    assert_true(pick_state.get("currentManager") == manager_email, f"pick 2 should belong to manager: {pick_state}")

    request("POST", f"/api/leagues/{league_id}/draft/picks", {"player": {
        "id": f"smoke-player-2-{suffix}",
        "name": "Smoke Test WR",
        "team": "Test State",
        "position": "WR",
        "conference": "Smoke",
        "projection": 17.2,
        "rank": 2,
    }}, token=manager_token, expected=(201,))
    request("POST", f"/api/leagues/{league_id}/draft/picks", {"player": {
        "id": f"smoke-player-bad-turn-{suffix}",
        "name": "Smoke Test Bad Turn",
        "team": "Test State",
        "position": "TE",
        "conference": "Smoke",
        "projection": 12.0,
        "rank": 99,
    }}, token=token, expected=(409,))
    snake_turn = request("POST", f"/api/leagues/{league_id}/draft/picks", {"player": {
        "id": f"smoke-player-3-{suffix}",
        "name": "Smoke Test QB",
        "team": "Test State",
        "position": "QB",
        "conference": "Smoke",
        "projection": 22.1,
        "rank": 3,
    }}, token=manager_token, expected=(201,))
    assert_true(len(snake_turn.get("picks", [])) == 3, f"snake turn pick was not recorded: {snake_turn}")
    assert_true(snake_turn.get("currentPick") == 4, f"snake turn did not advance to pick 4: {snake_turn}")
    assert_true(snake_turn.get("currentManager") == email, f"pick 4 should return to commissioner: {snake_turn}")

    undo_state = request("POST", f"/api/leagues/{league_id}/draft/undo", token=token)
    assert_true(len(undo_state.get("picks", [])) == 2, f"draft undo did not remove only the last pick: {undo_state}")
    assert_true(undo_state.get("currentPick") == 3, f"draft undo did not restore current pick 3: {undo_state}")
    assert_true(undo_state.get("currentManager") == manager_email, f"undo should put pick 3 back on manager: {undo_state}")

    request("GET", "/api/admin/ingest/cfbd/status", token=token, expected=(403,))

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
