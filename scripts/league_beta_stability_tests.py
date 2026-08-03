#!/usr/bin/env python3
"""Integration checks for league settings, join codes, and real player pools."""

from __future__ import annotations

import json
import os
import sys
import time
import urllib.error
import urllib.request


BASE_URL = os.environ.get("CFF_API_BASE_URL", "http://127.0.0.1:8080").rstrip("/")
PASSWORD = "BetaStability123!"


class TestFailure(RuntimeError):
    pass


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


def require(condition, message: str):
    if not condition:
        raise TestFailure(message)


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


def canonical(value):
    if isinstance(value, dict):
        return {key: canonical(value[key]) for key in sorted(value)}
    if isinstance(value, list):
        return [canonical(item) for item in value]
    return value


def main():
    suffix = str(int(time.time() * 1000))
    commissioner_email = f"beta-commissioner-{suffix}@example.com"
    invited_email = f"beta-invited-{suffix}@example.com"
    requester_email = f"beta-requester-{suffix}@example.com"

    commissioner_token = signup(commissioner_email)
    invited_token = signup(invited_email)
    requester_token = signup(requester_email)

    created = request(
        "POST",
        "/api/leagues",
        {
            "name": f"Beta League {suffix}",
            "teams": 4,
            "scoring": "ppr",
            "draftType": "snake",
            "invitedEmails": [],
            "rosterRules": {"qb": 1, "rb": 2, "wr": 2, "te": 1, "flex": 1, "bench": 4},
            "waiverRules": {"mode": "free_agency", "claimDeadline": "", "freeAgencyLocked": False},
            "tradeRules": {"commissionerApproval": False, "expirationHours": 48},
        },
        token=commissioner_token,
        expected=(201,),
    )
    league_id = created.get("id")
    require(league_id, f"league creation did not return an id: {created}")

    settings = {
        **created,
        "name": f"Verified Beta League {suffix}",
        "teams": 6,
        "scoring": "half_ppr",
        "scoringSettings": {
            "passingYardsPerPoint": 20,
            "passingTd": 5,
            "interception": -3,
            "rushingYardsPerPoint": 8,
            "rushingTd": 6,
            "receivingYardsPerPoint": 8,
            "receivingTd": 6,
            "reception": 0.5,
            "fumbleLost": -2,
            "twoPointConversion": 2,
        },
        "draftType": "snake",
        "draftDate": "2026-08-22T18:30",
        "notes": "Every league setting must survive a read-after-write check.",
        "rosterRules": {"qb": 2, "rb": 2, "wr": 3, "te": 1, "flex": 2, "bench": 8},
        "waiverRules": {
            "mode": "waivers",
            "claimDeadline": "2026-08-21T12:00",
            "freeAgencyLocked": True,
        },
        "tradeRules": {"commissionerApproval": True, "expirationHours": 72},
        "invitedEmails": [invited_email.upper()],
    }
    saved = request(
        "PUT",
        f"/api/leagues/{league_id}/settings",
        settings,
        token=commissioner_token,
    )

    fields = (
        "name",
        "teams",
        "scoring",
        "scoringSettings",
        "draftDate",
        "notes",
        "rosterRules",
        "waiverRules",
        "tradeRules",
    )
    for field in fields:
        require(
            canonical(saved.get(field)) == canonical(settings.get(field)),
            f"{field} did not persist: requested={settings.get(field)!r} saved={saved.get(field)!r}",
        )
    require(saved.get("invitedEmails") == [invited_email], f"invite emails were not canonicalized: {saved}")

    listed = request("GET", "/api/leagues", token=commissioner_token)
    persisted = next((league for league in listed if league.get("id") == league_id), None)
    require(persisted, "saved league disappeared from the commissioner list")
    for field in fields:
        require(
            canonical(persisted.get(field)) == canonical(saved.get(field)),
            f"{field} changed after a fresh list read: saved={saved.get(field)!r} listed={persisted.get(field)!r}",
        )

    join_info = request(
        "GET",
        f"/api/leagues/{league_id}/join-info",
        token=commissioner_token,
    )
    join_code = join_info.get("joinCode")
    require(join_code and len(join_code.replace("-", "")) == 8, f"invalid join code: {join_info}")

    invited_join = request(
        "POST",
        "/api/leagues/join",
        {"code": join_code},
        token=invited_token,
    )
    require(invited_join.get("id") == league_id, f"invited account was not activated: {invited_join}")

    pending = request(
        "POST",
        "/api/leagues/join",
        {"code": join_code.lower()},
        token=requester_token,
        expected=(202,),
    )
    require(pending.get("joinStatus") == "pending_approval", f"uninvited account did not create a request: {pending}")

    members = request(
        "GET",
        f"/api/leagues/{league_id}/members",
        token=commissioner_token,
    )
    requester = next((member for member in members if member.get("email") == requester_email), None)
    require(requester and requester.get("status") == "Pending", f"pending request is missing: {members}")

    request(
        "PUT",
        f"/api/leagues/{league_id}/members/{requester_email}",
        {"role": "member", "status": "Active", "teamName": "Beta Requester"},
        token=commissioner_token,
    )
    requester_leagues = request("GET", "/api/leagues", token=requester_token)
    require(
        any(league.get("id") == league_id for league in requester_leagues),
        "approved account cannot access the league",
    )

    roster = request("GET", f"/api/leagues/{league_id}/roster", token=commissioner_token)
    require(roster == [], f"a newly created league inherited a roster: {roster}")
    draft = request("GET", f"/api/leagues/{league_id}/draft", token=commissioner_token)
    require(draft.get("queue", []) == [], f"a newly created league inherited a draft queue: {draft}")

    player_pool = request(
        "GET",
        f"/api/leagues/{league_id}/player-pool",
        token=commissioner_token,
    )
    require(isinstance(player_pool, list), f"player pool is not a list: {player_pool}")
    sample_ids = {f"p-{index:03d}" for index in range(1, 13)}
    leaked = sorted(sample_ids.intersection({str(player.get("id")) for player in player_pool}))
    require(not leaked, f"demo players leaked into the production player pool: {leaked}")

    print(json.dumps({
        "status": "ok",
        "leagueId": league_id,
        "joinCode": join_code,
        "settingsVerified": list(fields),
        "invitedJoin": "active",
        "uninvitedJoin": "approved",
        "playerPoolCount": len(player_pool),
    }, indent=2))


if __name__ == "__main__":
    try:
        main()
    except TestFailure as exc:
        print(json.dumps({"status": "failed", "error": str(exc)}, indent=2), file=sys.stderr)
        raise SystemExit(1)
