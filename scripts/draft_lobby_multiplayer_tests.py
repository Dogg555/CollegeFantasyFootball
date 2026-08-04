#!/usr/bin/env python3
"""Production-image integration checks for multiplayer draft lobby and start flow."""

from __future__ import annotations

import json
import os
import sys
import time
import urllib.error
import urllib.request


BASE_URL = os.environ.get("CFF_API_BASE_URL", "http://127.0.0.1:8080").rstrip("/")
PASSWORD = "DraftLobby123!"


class TestFailure(RuntimeError):
    pass


def require(condition, message: str):
    if not condition:
        raise TestFailure(message)


def request(method: str, path: str, body=None, token: str = "", expected=(200,)):
    payload = None
    headers = {"Accept": "application/json"}
    if body is not None:
        payload = json.dumps(body).encode("utf-8")
        headers["Content-Type"] = "application/json"
    if token:
        headers["Authorization"] = f"Bearer {token}"
    req = urllib.request.Request(
        f"{BASE_URL}{path}",
        data=payload,
        headers=headers,
        method=method,
    )
    try:
        with urllib.request.urlopen(req, timeout=30) as response:
            status = response.getcode()
            text = response.read().decode("utf-8")
    except urllib.error.HTTPError as exc:
        status = exc.code
        text = exc.read().decode("utf-8")
    except urllib.error.URLError as exc:
        raise TestFailure(f"{method} {path} could not reach {BASE_URL}: {exc}") from exc
    if status not in expected:
        raise TestFailure(f"{method} {path} expected {expected}, got {status}: {text}")
    return json.loads(text) if text else {}


def signup(email: str) -> str:
    payload = request(
        "POST",
        "/api/auth/signup",
        {"email": email, "password": PASSWORD},
        expected=(201,),
    )
    token = payload.get("token")
    require(token, f"signup did not return a token for {email}: {payload}")
    return token


def league_payload(name: str, invited_email: str):
    return {
        "name": name,
        "teams": 4,
        "scoring": "ppr",
        "draftType": "snake",
        "draftDate": "",
        "notes": "Multiplayer draft lobby integration contract",
        "invitedEmails": [invited_email],
        "scoringSettings": {
            "passingYardsPerPoint": 25,
            "passingTd": 4,
            "interception": -2,
            "rushingYardsPerPoint": 10,
            "rushingTd": 6,
            "receivingYardsPerPoint": 10,
            "receivingTd": 6,
            "reception": 1,
            "fumbleLost": -2,
            "twoPointConversion": 2,
        },
        "rosterRules": {"qb": 1, "rb": 1, "wr": 1, "te": 0, "flex": 0, "bench": 1},
        "waiverRules": {"mode": "free_agency", "claimDeadline": "", "freeAgencyLocked": False},
        "tradeRules": {"commissionerApproval": False, "expirationHours": 48},
    }


def player(player_id: str, name: str, position: str):
    return {
        "id": player_id,
        "name": name,
        "team": "Contract State",
        "position": position,
        "conference": "Test",
        "class": "JR",
        "rank": 1,
        "projection": 20,
    }


def main():
    suffix = str(int(time.time() * 1000))
    commissioner_email = f"draft-commissioner-{suffix}@example.com"
    member_email = f"draft-member-{suffix}@example.com"
    commissioner_token = signup(commissioner_email)
    member_token = signup(member_email)

    created = request(
        "POST",
        "/api/leagues",
        league_payload(f"Draft Lobby {suffix}", member_email),
        token=commissioner_token,
        expected=(201,),
    )
    league_id = created.get("id")
    require(league_id, f"league creation did not return an id: {created}")

    joined = request(
        "POST",
        f"/api/leagues/{league_id}/join",
        token=member_token,
    )
    require(joined.get("id") == league_id, f"invited manager could not join league: {joined}")

    waiting = request("GET", f"/api/leagues/{league_id}/draft", token=member_token)
    require(waiting.get("status") == "not_started", f"draft state started on entry: {waiting}")
    require(waiting.get("lobbyOpen") is False, f"closed lobby reported open: {waiting}")
    require(not waiting.get("pickDeadline"), f"waiting lobby received a running clock: {waiting}")

    settings = {**created, "draftLobbyOpen": True, "draftLobbyStartedAt": "2026-08-04T02:00"}
    opened = request(
        "PUT",
        f"/api/leagues/{league_id}",
        settings,
        token=commissioner_token,
    )
    require(opened.get("draftLobbyOpen") is True, f"commissioner could not open lobby: {opened}")

    member_lobby = request("GET", f"/api/leagues/{league_id}/draft", token=member_token)
    require(member_lobby.get("lobbyOpen") is True, f"member cannot enter open lobby: {member_lobby}")
    require(member_lobby.get("status") == "not_started", f"entering lobby started draft: {member_lobby}")
    require(set(member_lobby.get("draftOrder", [])) == {commissioner_email, member_email}, f"draft order does not include both users: {member_lobby}")

    request(
        "POST",
        f"/api/leagues/{league_id}/draft/start",
        token=member_token,
        expected=(403,),
    )

    started = request(
        "POST",
        f"/api/leagues/{league_id}/draft/start",
        token=commissioner_token,
    )
    require(started.get("status") == "open", f"commissioner could not start draft: {started}")
    require(started.get("startedAt"), f"started draft is missing start timestamp: {started}")
    require(started.get("pickDeadline"), f"started draft is missing pick deadline: {started}")

    member_live = request("GET", f"/api/leagues/{league_id}/draft", token=member_token)
    require(member_live.get("status") == "open", f"member cannot observe live draft: {member_live}")
    require(member_live.get("startedAt") == started.get("startedAt"), "users received different draft start timestamps")

    order = started.get("draftOrder", [])
    require(len(order) == 2, f"unexpected draft order: {order}")
    tokens = {commissioner_email: commissioner_token, member_email: member_token}
    first_email, second_email = order

    wrong_email = second_email
    request(
        "POST",
        f"/api/leagues/{league_id}/draft/picks",
        {"player": player(f"wrong-{suffix}", "Wrong Turn", "QB")},
        token=tokens[wrong_email],
        expected=(409,),
    )

    first_pick = request(
        "POST",
        f"/api/leagues/{league_id}/draft/picks",
        {"player": player(f"first-{suffix}", "First Pick", "QB")},
        token=tokens[first_email],
        expected=(201,),
    )
    require(len(first_pick.get("picks", [])) == 1, f"first pick was not recorded: {first_pick}")
    require(first_pick.get("currentManager") == second_email, f"turn did not advance to second manager: {first_pick}")

    synced = request("GET", f"/api/leagues/{league_id}/draft", token=tokens[second_email])
    require(len(synced.get("picks", [])) == 1, f"second user did not receive first user's pick: {synced}")

    second_pick = request(
        "POST",
        f"/api/leagues/{league_id}/draft/picks",
        {"player": player(f"second-{suffix}", "Second Pick", "RB")},
        token=tokens[second_email],
        expected=(201,),
    )
    require(len(second_pick.get("picks", [])) == 2, f"second pick was not recorded: {second_pick}")

    request(
        "POST",
        f"/api/leagues/{league_id}/draft/reset",
        token=member_token,
        expected=(403,),
    )
    reset = request(
        "POST",
        f"/api/leagues/{league_id}/draft/reset",
        token=commissioner_token,
    )
    require(reset.get("status") == "not_started", f"reset draft did not return to lobby state: {reset}")
    require(reset.get("picks") == [], f"reset draft retained picks: {reset}")
    require(not reset.get("startedAt") and not reset.get("pickDeadline"), f"reset draft retained live timing: {reset}")

    request(
        "OPTIONS",
        f"/api/leagues/{league_id}/draft/start",
        expected=(204,),
    )

    print(json.dumps({
        "status": "ok",
        "leagueId": league_id,
        "members": order,
        "lobbyEntry": True,
        "explicitStart": True,
        "crossUserPickSync": True,
        "turnEnforcement": True,
        "resetToLobby": True,
    }, indent=2))


if __name__ == "__main__":
    try:
        main()
    except TestFailure as exc:
        print(json.dumps({"status": "failed", "error": str(exc)}, indent=2), file=sys.stderr)
        raise SystemExit(1)
