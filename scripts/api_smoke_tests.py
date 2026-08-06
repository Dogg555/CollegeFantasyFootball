#!/usr/bin/env python3
"""End-to-end API smoke checks for local Docker and deployed environments."""

from __future__ import annotations

import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request


BASE_URL = os.environ.get("CFF_API_BASE_URL", "http://127.0.0.1:8080").strip().rstrip("/")
PASSWORD = os.environ.get("CFF_SMOKE_PASSWORD", "SmokeTest123!")
EMAIL_PREFIX = os.environ.get("CFF_SMOKE_EMAIL_PREFIX", "smoke")
ADMIN_API_TOKEN = os.environ.get("CFF_ADMIN_API_TOKEN", "").strip()


class SmokeFailure(RuntimeError):
    pass


def request(method: str, path: str, body=None, token: str = "", expected=(200,), extra_headers=None):
    data = None
    headers = {"Accept": "application/json"}
    if extra_headers:
        headers.update(extra_headers)
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
    except json.JSONDecodeError:
        return {"raw": text}


def assert_true(condition, message: str):
    if not condition:
        raise SmokeFailure(message)


def smoke_player(suffix: str, player_id: str, name: str, position: str, projection: float = 10):
    return {
        "id": f"{player_id}-{suffix}",
        "name": name,
        "team": "Test State",
        "position": position,
        "conference": "Smoke",
        "projection": projection,
        "rank": 99,
    }


def operation_key(suffix: str, name: str) -> str:
    return f"smoke-{suffix}-{name}"


def main():
    suffix = str(int(time.time()))
    email = f"{EMAIL_PREFIX}+{suffix}@example.com"
    manager_email = f"{EMAIL_PREFIX}-manager+{suffix}@example.com"

    health = request("GET", "/health")
    assert_true(health.get("status") in {"ok", "degraded"}, f"unexpected health payload: {health}")

    api_health = request("GET", "/api/health")
    assert_true(api_health.get("service") == "college-ff-api", f"unexpected API health payload: {api_health}")
    request("GET", "/api/leagues", expected=(401,))

    signup = request("POST", "/api/auth/signup", {"email": email, "password": PASSWORD}, expected=(201,))
    token = signup.get("token")
    assert_true(token, f"signup did not return token: {signup}")

    validate = request("GET", "/api/auth/validate", token=token)
    assert_true(validate.get("valid") is True, f"token did not validate: {validate}")

    login = request("POST", "/api/auth/login", {"email": email, "password": PASSWORD})
    login_token = login.get("token")
    assert_true(login_token, f"login did not return token: {login}")
    request("POST", "/api/auth/logout", {}, token=login_token)
    request("GET", "/api/auth/validate", token=login_token, expected=(401,))

    league_payload = {
        "name": f"Smoke League {suffix}",
        "teams": 10,
        "scoring": "ppr",
        "draftType": "snake",
        "invitedEmails": [manager_email],
        "rosterRules": {"qb": 0, "rb": 0, "wr": 0, "te": 0, "flex": 0, "bench": 8},
        "waiverRules": {
            "mode": "waivers",
            "claimDeadline": "2000-01-01T00:00:00Z",
            "freeAgencyLocked": True,
        },
        "tradeRules": {
            "commissionerApproval": False,
            "expirationHours": 48,
        },
    }
    league = request("POST", "/api/leagues", league_payload, token=token, expected=(201,))
    league_id = league.get("id")
    assert_true(league_id, f"league creation did not return an ID: {league}")

    leagues = request("GET", "/api/leagues", token=token)
    assert_true(any(item.get("id") == league_id for item in leagues), "created league is missing from list")

    settings_update = {**league, "notes": "smoke settings update"}
    updated = request("PUT", f"/api/leagues/{league_id}", settings_update, token=token)
    assert_true(updated.get("notes") == "smoke settings update", f"league update failed: {updated}")

    manager_signup = request("POST", "/api/auth/signup", {"email": manager_email, "password": PASSWORD}, expected=(201,))
    manager_token = manager_signup.get("token")
    assert_true(manager_token, f"manager signup did not return token: {manager_signup}")

    joined = request("POST", f"/api/leagues/{league_id}/join", {}, token=manager_token, expected=(200, 202))
    assert_true(joined.get("id") == league_id, f"manager could not request league access: {joined}")
    if joined.get("joinStatus") == "pending_approval":
        approved = request(
            "PUT",
            f"/api/leagues/{league_id}/members/{urllib.parse.quote(manager_email, safe='')}",
            {"status": "active", "role": "member"},
            token=token,
        )
        assert_true(
            any(member.get("email") == manager_email and str(member.get("status", "")).lower() == "active" for member in approved),
            f"manager approval failed: {approved}",
        )
    else:
        assert_true(
            joined.get("joinStatus") == "active" or joined.get("id") == league_id,
            f"manager could not join invited league: {joined}",
        )

    members = request("GET", f"/api/leagues/{league_id}/members", token=token)
    member_emails = {member.get("email") for member in members}
    assert_true({email, manager_email}.issubset(member_emails), f"joined members missing: {members}")

    lobby_league = request("PUT", f"/api/leagues/{league_id}", {**updated, "draftLobbyOpen": True}, token=token)
    assert_true(lobby_league.get("draftLobbyOpen") is True, f"draft lobby did not open: {lobby_league}")

    draft_state = request("GET", f"/api/leagues/{league_id}/draft", token=token)
    order_state = request(
        "PUT",
        f"/api/leagues/{league_id}/draft/order",
        {"draftOrder": [email, manager_email], "expectedVersion": draft_state.get("version") or 0},
        token=token,
    )
    assert_true(order_state.get("draftOrder") == [email, manager_email], f"draft order was not saved: {order_state}")
    request("POST", f"/api/leagues/{league_id}/draft/readiness", {"ready": True}, token=token)
    draft_state = request("POST", f"/api/leagues/{league_id}/draft/readiness", {"ready": True}, token=manager_token)
    draft_state = request(
        "POST",
        f"/api/leagues/{league_id}/draft/start",
        {"expectedVersion": draft_state.get("version") or 0},
        token=token,
    )
    assert_true(draft_state.get("status") == "open", f"draft did not start: {draft_state}")

    commissioner_player = smoke_player(suffix, "smoke-rb", "Smoke Test RB", "RB", 18.4)
    manager_player = smoke_player(suffix, "smoke-wr", "Smoke Test WR", "WR", 17.2)
    extra_player = smoke_player(suffix, "smoke-qb", "Smoke Test QB", "QB", 22.1)

    first_pick = request(
        "POST",
        f"/api/leagues/{league_id}/draft/picks",
        {
            "player": commissioner_player,
            "expectedPick": draft_state.get("currentPick"),
            "expectedVersion": draft_state.get("version") or 0,
        },
        token=token,
        expected=(201,),
    )
    assert_true(first_pick.get("currentManager") == manager_email, f"pick 2 should belong to manager: {first_pick}")

    second_pick = request(
        "POST",
        f"/api/leagues/{league_id}/draft/picks",
        {
            "player": manager_player,
            "expectedPick": first_pick.get("currentPick"),
            "expectedVersion": first_pick.get("version") or 0,
        },
        token=manager_token,
        expected=(201,),
    )
    request(
        "POST",
        f"/api/leagues/{league_id}/draft/picks",
        {
            "player": smoke_player(suffix, "bad-turn", "Smoke Bad Turn", "TE"),
            "expectedPick": second_pick.get("currentPick"),
            "expectedVersion": second_pick.get("version") or 0,
        },
        token=token,
        expected=(409,),
    )
    snake_turn = request(
        "POST",
        f"/api/leagues/{league_id}/draft/picks",
        {
            "player": extra_player,
            "expectedPick": second_pick.get("currentPick"),
            "expectedVersion": second_pick.get("version") or 0,
        },
        token=manager_token,
        expected=(201,),
    )
    assert_true(snake_turn.get("currentManager") == email, f"pick 4 should return to commissioner: {snake_turn}")

    undo_state = request(
        "POST",
        f"/api/leagues/{league_id}/draft/undo",
        {"expectedVersion": snake_turn.get("version") or 0},
        token=token,
    )
    assert_true(len(undo_state.get("picks") or []) == 2, f"draft undo did not remove last pick: {undo_state}")

    pending_waiver_player = smoke_player(suffix, "waiver-pending", "Smoke Pending Waiver", "QB", 9.8)
    pending_waiver = request(
        "POST",
        f"/api/leagues/{league_id}/waivers",
        {"addPlayer": pending_waiver_player, "dropPlayerId": commissioner_player["id"]},
        token=token,
        expected=(200, 201),
        extra_headers={"Idempotency-Key": operation_key(suffix, "waiver-create")},
    )
    assert_true(pending_waiver.get("id"), f"pending waiver missing id: {pending_waiver}")

    request("POST", f"/api/leagues/{league_id}/score/week/1", {"season": 2026}, token=token)
    finalized = request("POST", f"/api/leagues/{league_id}/score/week/1/finalize", {}, token=token)
    assert_true(
        any(str(matchup.get("status", "")).lower() == "final" for matchup in finalized),
        f"week was not finalized: {finalized}",
    )

    request(
        "POST",
        f"/api/leagues/{league_id}/roster",
        {"player": smoke_player(suffix, "locked-add", "Smoke Locked Add", "TE")},
        token=token,
        expected=(409,),
        extra_headers={"Idempotency-Key": operation_key(suffix, "locked-add")},
    )
    request(
        "POST",
        f"/api/leagues/{league_id}/roster/drop",
        {"playerId": commissioner_player["id"]},
        token=token,
        expected=(409,),
        extra_headers={"Idempotency-Key": operation_key(suffix, "locked-drop")},
    )
    request(
        "POST",
        f"/api/leagues/{league_id}/roster/{commissioner_player['id']}/slot",
        {"slot": "bench"},
        token=token,
        expected=(409,),
        extra_headers={"Idempotency-Key": operation_key(suffix, "locked-slot")},
    )
    request(
        "POST",
        f"/api/leagues/{league_id}/waivers",
        {"addPlayer": smoke_player(suffix, "locked-waiver", "Smoke Locked Waiver", "TE"), "dropPlayerId": commissioner_player["id"]},
        token=token,
        expected=(409,),
        extra_headers={"Idempotency-Key": operation_key(suffix, "locked-waiver")},
    )
    request(
        "POST",
        f"/api/leagues/{league_id}/waivers/{pending_waiver['id']}/process",
        {},
        token=token,
        expected=(409,),
        extra_headers={"Idempotency-Key": operation_key(suffix, "waiver-process")},
    )
    request(
        "POST",
        f"/api/leagues/{league_id}/waivers/{pending_waiver['id']}/status",
        {"status": "Cancelled"},
        token=token,
        expected=(200,),
        extra_headers={"Idempotency-Key": operation_key(suffix, "waiver-cancel")},
    )
    request(
        "POST",
        f"/api/leagues/{league_id}/trades",
        {
            "offerPlayer": commissioner_player,
            "requestPlayer": manager_player,
            "requestPlayerName": manager_player["name"],
            "targetManager": manager_email,
        },
        token=token,
        expected=(409,),
        extra_headers={"Idempotency-Key": operation_key(suffix, "locked-trade")},
    )

    request("GET", "/api/admin/ingest/cfbd/status", token=token, expected=(403,))
    if ADMIN_API_TOKEN:
        admin_status = request("GET", "/api/admin/ingest/cfbd/status", token=ADMIN_API_TOKEN)
        assert_true(isinstance(admin_status, dict), f"unexpected admin ingestion status: {admin_status}")

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
        "managerEmail": manager_email,
        "leagueId": league_id,
    }, indent=2))


if __name__ == "__main__":
    try:
        main()
    except SmokeFailure as exc:
        print(json.dumps({"status": "failed", "error": str(exc)}, indent=2), file=sys.stderr)
        raise SystemExit(1)
