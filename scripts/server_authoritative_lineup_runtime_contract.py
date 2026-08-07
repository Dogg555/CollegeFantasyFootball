#!/usr/bin/env python3
"""Production PostgreSQL contract for atomic whole-lineup saves."""
from __future__ import annotations

import hashlib
import json
import os
import time
import urllib.error
import urllib.request

import psycopg

BASE_URL = os.environ.get("CFF_API_BASE_URL", "http://127.0.0.1:8080").rstrip("/")
DB_URL = os.environ["DB_URL"]
PASSWORD = os.environ.get("CFF_CONTRACT_PASSWORD", "Lineup-Contract-Password-77!")
RUN_KEY = os.environ.get("CFF_LINEUP_RUN_KEY", str(int(time.time())))
OWNER = f"lineup-owner-{RUN_KEY}@example.test"
OUTSIDER = f"lineup-outsider-{RUN_KEY}@example.test"
LEAGUE_ID = f"lineup-{RUN_KEY}"
SEASON = 2026
WEEK = 1


def request(method: str, path: str, body: dict | None = None, token: str = "", key: str = "") -> tuple[int, dict]:
    headers = {"Accept": "application/json", "Origin": "https://frontend.example.test"}
    data = None
    if body is not None:
        headers["Content-Type"] = "application/json"
        data = json.dumps(body).encode()
    if token:
        headers["Authorization"] = f"Bearer {token}"
    if key:
        headers["Idempotency-Key"] = key
    req = urllib.request.Request(f"{BASE_URL}{path}", data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=30) as response:
            return response.status, json.loads(response.read().decode() or "{}")
    except urllib.error.HTTPError as error:
        return error.code, json.loads(error.read().decode() or "{}")


def wait_for_api() -> None:
    for _ in range(90):
        try:
            status, payload = request("GET", "/health")
            if status == 200 and payload.get("status") == "ok" and payload.get("database") == "ok":
                return
        except Exception:
            pass
        time.sleep(1)
    raise AssertionError("API did not become healthy")


def account(email: str) -> str:
    status, _ = request("POST", "/api/auth/signup", {"email": email, "password": PASSWORD})
    assert status in (200, 201, 202, 409), (status, email)
    status, payload = request("POST", "/api/auth/login", {"email": email, "password": PASSWORD})
    assert status == 200, (status, payload)
    return payload["token"]


def seed() -> None:
    rules = json.dumps({"qb": 1, "rb": 1, "wr": 1, "te": 0, "flex": 1, "k": 0, "def": 0, "bench": 2})
    with psycopg.connect(DB_URL, autocommit=True) as connection:
        with connection.cursor() as cursor:
            cursor.execute(
                """
                INSERT INTO leagues (id, account_email, name, team_count, scoring, draft_type, roster_rules)
                VALUES (%s, %s, 'Atomic Lineup Contract', 4, 'ppr', 'snake', %s::jsonb)
                """,
                (LEAGUE_ID, OWNER, rules),
            )
            cursor.execute(
                """
                INSERT INTO league_members (league_id, email, team_name, role, status, joined_at)
                VALUES (%s, %s, 'Owner Team', 'commissioner', 'active', NOW())
                """,
                (LEAGUE_ID, OWNER),
            )
            players = [
                ("qb-start", "QB Start", "QB", "Open State", "qb"),
                ("rb-start", "RB Start", "RB", "Locked State", "rb"),
                ("wr-start", "WR Start", "WR", "Open State", "wr"),
                ("te-flex", "TE Flex", "TE", "Open State", "flex"),
                ("qb-bench", "QB Bench", "QB", "Open State", "bench"),
                ("rb-bench", "RB Bench", "RB", "Locked State", "bench"),
            ]
            for player_id, name, position, team, slot in players:
                snapshot = json.dumps({"id": player_id, "name": name, "position": position, "team": team})
                cursor.execute(
                    """
                    INSERT INTO rosters (league_id, manager_email, player_id, player_snapshot, roster_slot, acquired_via)
                    VALUES (%s, %s, %s, %s::jsonb, %s, 'draft')
                    """,
                    (LEAGUE_ID, OWNER, player_id, snapshot, slot),
                )
            cursor.execute(
                """
                INSERT INTO schedule_week_states (league_id, season, week, status, lineup_deadline)
                VALUES (%s, %s, %s, 'open', NOW() + INTERVAL '1 day')
                """,
                (LEAGUE_ID, SEASON, WEEK),
            )
            cursor.execute(
                """
                INSERT INTO games (id, season, week, start_date, home_team, away_team)
                VALUES (%s, %s, %s, NOW() - INTERVAL '1 hour', 'Locked State', 'Other State')
                """,
                (int(hashlib.sha256(RUN_KEY.encode()).hexdigest()[:12], 16) % 2_000_000_000, SEASON, WEEK),
            )


def slots() -> dict[str, str]:
    with psycopg.connect(DB_URL) as connection:
        with connection.cursor() as cursor:
            cursor.execute(
                "SELECT player_id, roster_slot FROM rosters WHERE league_id = %s AND lower(manager_email) = lower(%s)",
                (LEAGUE_ID, OWNER),
            )
            return dict(cursor.fetchall())


def lineup(assignments: list[object], version: int) -> dict:
    return {
        "action": "lineup",
        "expectedVersion": version,
        "season": SEASON,
        "week": WEEK,
        "assignments": assignments,
    }


def main() -> None:
    wait_for_api()
    owner_token = account(OWNER)
    outsider_token = account(OUTSIDER)
    seed()

    status, state = request("GET", f"/api/leagues/{LEAGUE_ID}/roster/state", token=owner_token)
    assert status == 200, (status, state)
    version = state["version"]

    valid = [
        {"playerId": "qb-start", "slot": "bench"},
        {"playerId": "rb-start", "slot": "rb"},
        {"playerId": "wr-start", "slot": "wr"},
        {"playerId": "te-flex", "slot": "flex"},
        {"playerId": "qb-bench", "slot": "qb"},
        {"playerId": "rb-bench", "slot": "bench"},
    ]
    status, saved = request(
        "POST", f"/api/leagues/{LEAGUE_ID}/roster/transactions",
        lineup(valid, version), owner_token, f"valid-{RUN_KEY}",
    )
    assert status == 200, (status, saved)
    assert saved["action"] == "lineup"
    assert saved["version"] == version + 1
    assert saved["changedCount"] == 2
    assert slots()["qb-start"] == "bench"
    assert slots()["qb-bench"] == "qb"

    status, replay = request(
        "POST", f"/api/leagues/{LEAGUE_ID}/roster/transactions",
        lineup(valid, version), owner_token, f"valid-{RUN_KEY}",
    )
    assert status == 200, (status, replay)
    assert replay["version"] == saved["version"]
    assert replay.get("idempotentReplay") is True

    before_invalid = slots()
    invalid = [dict(item) for item in valid]
    invalid[-1]["playerId"] = "qb-start"
    status, rejected = request(
        "POST", f"/api/leagues/{LEAGUE_ID}/roster/transactions",
        lineup(invalid, saved["version"]), owner_token, f"invalid-{RUN_KEY}",
    )
    assert status == 409 and rejected.get("code") == "invalid_lineup", (status, rejected)
    assert slots() == before_invalid

    malformed_scalar: list[object] = [dict(item) for item in valid]
    malformed_scalar[0] = "qb-start:bench"
    status, malformed = request(
        "POST", f"/api/leagues/{LEAGUE_ID}/roster/transactions",
        lineup(malformed_scalar, saved["version"]), owner_token, f"malformed-scalar-{RUN_KEY}",
    )
    assert status == 409 and malformed.get("code") == "invalid_lineup", (status, malformed)
    assert slots() == before_invalid

    malformed_slot: list[object] = [dict(item) for item in valid]
    malformed_slot[0]["slot"] = {"name": "bench"}
    status, malformed = request(
        "POST", f"/api/leagues/{LEAGUE_ID}/roster/transactions",
        lineup(malformed_slot, saved["version"]), owner_token, f"malformed-slot-{RUN_KEY}",
    )
    assert status == 409 and malformed.get("code") == "invalid_lineup", (status, malformed)
    assert slots() == before_invalid

    oversized_week = lineup(valid, saved["version"])
    oversized_week["week"] = 2**63
    status, range_error = request(
        "POST", f"/api/leagues/{LEAGUE_ID}/roster/transactions",
        oversized_week, owner_token, f"oversized-week-{RUN_KEY}",
    )
    assert status == 400 and range_error.get("code") == "lineup_week_required", (status, range_error)
    assert slots() == before_invalid

    locked_change = [dict(item) for item in valid]
    for item in locked_change:
        if item["playerId"] == "rb-start": item["slot"] = "bench"
        if item["playerId"] == "rb-bench": item["slot"] = "rb"
    status, locked = request(
        "POST", f"/api/leagues/{LEAGUE_ID}/roster/transactions",
        lineup(locked_change, saved["version"]), owner_token, f"locked-{RUN_KEY}",
    )
    assert status == 409 and locked.get("code") == "lineup_player_locked", (status, locked)
    assert slots() == before_invalid

    with psycopg.connect(DB_URL, autocommit=True) as connection:
        with connection.cursor() as cursor:
            cursor.execute(
                "UPDATE schedule_week_states SET lineup_deadline = NOW() - INTERVAL '1 minute' WHERE league_id = %s AND season = %s AND week = %s",
                (LEAGUE_ID, SEASON, WEEK),
            )
    deadline_change = [dict(item) for item in valid]
    for item in deadline_change:
        if item["playerId"] == "qb-start": item["slot"] = "qb"
        if item["playerId"] == "qb-bench": item["slot"] = "bench"
    status, expired = request(
        "POST", f"/api/leagues/{LEAGUE_ID}/roster/transactions",
        lineup(deadline_change, saved["version"]), owner_token, f"expired-{RUN_KEY}",
    )
    assert status == 409 and expired.get("code") == "lineup_locked", (status, expired)
    assert slots() == before_invalid

    status, denied = request(
        "POST", f"/api/leagues/{LEAGUE_ID}/roster/transactions",
        lineup(valid, saved["version"]), outsider_token, f"outsider-{RUN_KEY}",
    )
    assert status == 403 and denied.get("code") == "league_membership_required", (status, denied)

    status, unauthenticated = request(
        "POST", f"/api/leagues/{LEAGUE_ID}/roster/transactions",
        lineup(valid, saved["version"]), key=f"unauth-{RUN_KEY}",
    )
    assert status == 401 and unauthenticated.get("code") == "authentication_required", (status, unauthenticated)

    with psycopg.connect(DB_URL) as connection:
        with connection.cursor() as cursor:
            cursor.execute(
                "SELECT version, jsonb_array_length(lineup_snapshot) FROM lineup_week_states WHERE league_id = %s AND season = %s AND week = %s AND lower(manager_email) = lower(%s)",
                (LEAGUE_ID, SEASON, WEEK, OWNER),
            )
            lineup_version, snapshot_size = cursor.fetchone()
            assert lineup_version == 1
            assert snapshot_size == 6

    print("server-authoritative lineup production runtime contract passed")


if __name__ == "__main__":
    main()
