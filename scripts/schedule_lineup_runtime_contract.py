#!/usr/bin/env python3

from __future__ import annotations

import json
import os
import sys
import time
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from typing import Any

import psycopg


API_BASE = os.environ.get("CFF_API_BASE_URL", "http://127.0.0.1:8080").rstrip("/")
DB_URL = os.environ.get("DB_URL", "postgresql://postgres:postgres@127.0.0.1:5432/cff")
ORIGIN = os.environ.get("CFF_CONTRACT_ORIGIN", "https://frontend.example.test")
PASSWORD = os.environ.get("CFF_CONTRACT_PASSWORD", "Schedule-Contract-Password-2026!")
RUN_KEY = os.environ.get("CFF_SCHEDULE_RUN_KEY", str(int(time.time())))
SEASON = 2026


@dataclass
class Response:
    status: int
    data: Any


def request(method: str, path: str, token: str = "", payload: Any = None, key: str = "") -> Response:
    body = None if payload is None else json.dumps(payload).encode("utf-8")
    headers = {"Accept": "application/json", "Origin": ORIGIN}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    if body is not None:
        headers["Content-Type"] = "application/json"
    if key:
        headers["Idempotency-Key"] = key
    req = urllib.request.Request(f"{API_BASE}{path}", data=body, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=30) as response:
            raw = response.read().decode("utf-8")
            return Response(response.status, json.loads(raw) if raw else None)
    except urllib.error.HTTPError as error:
        raw = error.read().decode("utf-8")
        try:
            data = json.loads(raw) if raw else None
        except json.JSONDecodeError:
            data = {"raw": raw}
        return Response(error.code, data)
    except urllib.error.URLError as error:
        raise RuntimeError(f"{method} {path} could not reach {API_BASE}: {error}") from error


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def wait_for_api() -> None:
    last = "no response"
    for _ in range(90):
        try:
            response = request("GET", "/api/auth/status")
            if response.status == 200 and response.data.get("ready") is True:
                return
            last = f"HTTP {response.status}: {response.data!r}"
        except Exception as error:  # noqa: BLE001 - retained for diagnostics
            last = str(error)
        time.sleep(2)
    raise RuntimeError(f"API did not become ready: {last}")


def signup(email: str) -> str:
    response = request(
        "POST",
        "/api/auth/signup",
        payload={"email": email, "password": PASSWORD},
    )
    require(response.status == 201, f"signup {email} failed: {response.status} {response.data}")
    token = str(response.data.get("token", ""))
    require(token.startswith("token-"), f"signup did not return a bearer token for {email}: {response.data}")
    return token


def seed() -> tuple[str, list[str], list[str]]:
    emails = [f"schedule-{RUN_KEY}-{index}@example.test" for index in range(1, 5)]
    tokens = [signup(email) for email in emails]
    players = [f"schedule-player-{RUN_KEY}-{index}" for index in range(1, 5)]

    created = request(
        "POST",
        "/api/leagues",
        tokens[0],
        {
            "name": f"Schedule contract {RUN_KEY}",
            "teams": 4,
            "scoring": "ppr",
            "draftType": "snake",
            "invitedEmails": emails[1:],
            "rosterRules": {"qb": 1, "rb": 0, "wr": 0, "te": 0, "flex": 0, "bench": 1},
        },
        f"schedule-create-{RUN_KEY}",
    )
    require(created.status == 201, f"league creation failed: {created.status} {created.data}")
    league_id = str(created.data.get("id", ""))
    require(league_id, f"league creation did not return an ID: {created.data}")

    try:
        game_id = 900000000 + int(RUN_KEY.split("-")[0]) % 90000000
    except ValueError:
        game_id = 900000000 + abs(hash(RUN_KEY)) % 90000000

    scoring_settings = {
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
    }
    roster_rules = {"qb": 1, "rb": 0, "wr": 0, "te": 0, "flex": 0, "bench": 1}

    with psycopg.connect(DB_URL) as connection:
        with connection.cursor() as cursor:
            cursor.execute(
                "UPDATE league_members SET status = 'active', joined_at = COALESCE(joined_at, NOW()), "
                "team_name = CASE lower(email) "
                "WHEN %s THEN 'Alpha' WHEN %s THEN 'Bravo' WHEN %s THEN 'Charlie' ELSE 'Delta' END, "
                "updated_at = NOW() WHERE league_id = %s",
                (*[email.lower() for email in emails[:3]], league_id),
            )
            cursor.execute(
                "UPDATE leagues SET roster_rules = %s::jsonb, scoring_settings = %s::jsonb, updated_at = NOW() "
                "WHERE id = %s",
                (json.dumps(roster_rules), json.dumps(scoring_settings), league_id),
            )
            cursor.execute("DELETE FROM rosters WHERE league_id = %s", (league_id,))
            cursor.execute(
                "INSERT INTO games (id, season, week, season_type, home_team, away_team, updated_at) "
                "VALUES (%s, %s, 1, 'regular', 'Home', 'Away', NOW()) "
                "ON CONFLICT (id) DO UPDATE SET season = EXCLUDED.season, week = EXCLUDED.week, updated_at = NOW()",
                (game_id, SEASON),
            )
            for index, (email, player_id) in enumerate(zip(emails, players, strict=True), start=1):
                snapshot = {
                    "id": player_id,
                    "playerId": player_id,
                    "name": f"Contract Quarterback {index}",
                    "position": "QB",
                    "team": f"School {index}",
                }
                cursor.execute(
                    "INSERT INTO players (id, full_name, position, team, conference, raw, updated_at) "
                    "VALUES (%s, %s, 'QB', %s, 'Test', %s::jsonb, NOW()) "
                    "ON CONFLICT (id) DO UPDATE SET full_name = EXCLUDED.full_name, raw = EXCLUDED.raw, updated_at = NOW()",
                    (player_id, snapshot["name"], snapshot["team"], json.dumps(snapshot)),
                )
                cursor.execute(
                    "INSERT INTO rosters "
                    "(league_id, manager_email, player_id, player_snapshot, roster_slot, acquired_via, acquired_at) "
                    "VALUES (%s, %s, %s, %s::jsonb, 'qb', 'draft', NOW())",
                    (league_id, email, player_id, json.dumps(snapshot)),
                )
                cursor.execute(
                    "INSERT INTO player_stats "
                    "(player_id, season, week, team, conference, category, stat_name, stat_value, game_id, updated_at) "
                    "VALUES (%s, %s, 1, %s, 'Test', 'passing', 'passingYards', %s, %s, NOW()) "
                    "ON CONFLICT (player_id, season, week, category, stat_name, game_id) "
                    "DO UPDATE SET stat_value = EXCLUDED.stat_value, updated_at = NOW()",
                    (player_id, SEASON, snapshot["team"], index * 100, game_id),
                )
            cursor.execute(
                "SELECT COUNT(*) FROM league_members WHERE league_id = %s AND status = 'active'",
                (league_id,),
            )
            require(int(cursor.fetchone()[0]) == 4, "not all four managers became active")
        connection.commit()
    return league_id, emails, tokens


def main() -> None:
    wait_for_api()
    league_id, emails, tokens = seed()
    commissioner = tokens[0]

    state = request("GET", f"/api/leagues/{league_id}/schedule/state?season={SEASON}&week=1", commissioner)
    require(state.status == 200, f"initial state failed: {state.status} {state.data}")
    require(state.data["scheduleVersion"] == 0, "initial schedule version must be zero")

    generate_payload = {
        "action": "generate",
        "season": SEASON,
        "week": 1,
        "weeks": 3,
        "expectedVersion": 0,
    }
    keys = [f"schedule-generate-{RUN_KEY}-a", f"schedule-generate-{RUN_KEY}-b"]
    with ThreadPoolExecutor(max_workers=2) as executor:
        futures = [
            executor.submit(
                request,
                "POST",
                f"/api/leagues/{league_id}/schedule/transactions",
                commissioner,
                generate_payload,
                key,
            )
            for key in keys
        ]
        results = [future.result() for future in futures]

    statuses = sorted(result.status for result in results)
    require(statuses == [200, 409], f"schedule race must be one 200 and one 409: {statuses}")
    winner_index = next(index for index, result in enumerate(results) if result.status == 200)
    winner = results[winner_index]
    loser = results[1 - winner_index]
    require(loser.data.get("code") == "schedule_state_conflict", f"unexpected race loser: {loser.data}")
    schedule_ids = [matchup["id"] for matchup in winner.data["schedule"]]
    require(len(schedule_ids) == 6 and len(set(schedule_ids)) == 6, "three four-team weeks need six stable IDs")

    replay = request(
        "POST",
        f"/api/leagues/{league_id}/schedule/transactions",
        commissioner,
        generate_payload,
        keys[winner_index],
    )
    require(replay.status == 200 and replay.data.get("idempotentReplay") is True, f"generate replay failed: {replay.data}")
    require([item["id"] for item in replay.data["schedule"]] == schedule_ids, "replay changed matchup IDs")

    unchanged = request(
        "POST",
        f"/api/leagues/{league_id}/schedule/transactions",
        commissioner,
        {**generate_payload, "expectedVersion": 1},
        f"schedule-unchanged-{RUN_KEY}",
    )
    require(unchanged.status == 200 and unchanged.data.get("unchanged") is True, f"unchanged generation failed: {unchanged.data}")
    require(unchanged.data["scheduleVersion"] == 1, "unchanged generation advanced version")

    future_deadline = request(
        "POST",
        f"/api/leagues/{league_id}/schedule/transactions",
        commissioner,
        {
            "action": "set_deadline",
            "season": SEASON,
            "week": 1,
            "lineupDeadline": "2030-09-01T16:00:00Z",
            "expectedVersion": 1,
        },
        f"schedule-deadline-future-{RUN_KEY}",
    )
    require(future_deadline.status == 200 and future_deadline.data["scheduleVersion"] == 2, f"future deadline failed: {future_deadline.data}")

    locked = request(
        "POST",
        f"/api/leagues/{league_id}/schedule/transactions",
        tokens[1],
        {"action": "lock", "season": SEASON, "week": 1, "expectedVersion": 2},
        f"schedule-lock-{RUN_KEY}",
    )
    require(locked.status == 200 and locked.data["scheduleVersion"] == 3, f"manual lock failed: {locked.data}")

    blocked_slot = request(
        "POST",
        f"/api/leagues/{league_id}/roster/schedule-player-{RUN_KEY}-2/slot",
        tokens[1],
        {"slot": "bench"},
    )
    require(blocked_slot.status == 409 and blocked_slot.data.get("code") == "lineup_locked", f"locked starter moved: {blocked_slot.data}")

    unlocked = request(
        "POST",
        f"/api/leagues/{league_id}/schedule/transactions",
        tokens[1],
        {"action": "unlock", "season": SEASON, "week": 1, "expectedVersion": 3},
        f"schedule-unlock-{RUN_KEY}",
    )
    require(unlocked.status == 200 and unlocked.data["scheduleVersion"] == 4, f"manual unlock failed: {unlocked.data}")

    past_deadline = request(
        "POST",
        f"/api/leagues/{league_id}/schedule/transactions",
        commissioner,
        {
            "action": "set_deadline",
            "season": SEASON,
            "week": 1,
            "lineupDeadline": "2020-09-01T16:00:00Z",
            "expectedVersion": 4,
        },
        f"schedule-deadline-past-{RUN_KEY}",
    )
    require(past_deadline.status == 200 and past_deadline.data["scheduleVersion"] == 5, f"past deadline failed: {past_deadline.data}")

    auto_locked = request("GET", f"/api/leagues/{league_id}/schedule/state?season={SEASON}&week=1", tokens[2])
    require(auto_locked.status == 200 and auto_locked.data.get("deadlineAutoLocked") is True, f"deadline auto-lock failed: {auto_locked.data}")
    require(auto_locked.data["scheduleVersion"] == 6 and auto_locked.data["allLocked"] is True, "deadline lock state incorrect")

    late_unlock = request(
        "POST",
        f"/api/leagues/{league_id}/schedule/transactions",
        tokens[2],
        {"action": "unlock", "season": SEASON, "week": 1, "expectedVersion": 6},
        f"schedule-late-unlock-{RUN_KEY}",
    )
    require(late_unlock.status == 409 and late_unlock.data.get("code") == "lineup_deadline_passed", f"late unlock succeeded: {late_unlock.data}")

    scoring_state = request("GET", f"/api/leagues/{league_id}/scoring/state?season={SEASON}&week=1", commissioner)
    require(scoring_state.status == 200, f"scoring state failed: {scoring_state.data}")
    scored = request(
        "POST",
        f"/api/leagues/{league_id}/scoring/transactions",
        commissioner,
        {"action": "score", "season": SEASON, "week": 1, "expectedVersion": scoring_state.data["version"]},
        f"schedule-score-{RUN_KEY}",
    )
    require(scored.status == 200, f"score failed: {scored.status} {scored.data}")

    scoring_locked = request("GET", f"/api/leagues/{league_id}/schedule/state?season={SEASON}&week=1", commissioner)
    require(scoring_locked.status == 200 and scoring_locked.data["scheduleVersion"] == 7, f"scoring lock version incorrect: {scoring_locked.data}")
    reasons = {lineup["lockReason"] for lineup in scoring_locked.data["lineups"]}
    require(reasons == {"scoring"}, f"lineups not promoted to scoring lock: {reasons}")

    regenerate = request(
        "POST",
        f"/api/leagues/{league_id}/schedule/transactions",
        commissioner,
        {"action": "generate", "season": SEASON, "week": 1, "weeks": 4, "expectedVersion": 7},
        f"schedule-regenerate-{RUN_KEY}",
    )
    require(regenerate.status == 409 and regenerate.data.get("code") == "schedule_locked", f"locked schedule regenerated: {regenerate.data}")

    finalized = request(
        "POST",
        f"/api/leagues/{league_id}/scoring/transactions",
        commissioner,
        {"action": "finalize", "season": SEASON, "week": 1, "expectedVersion": scored.data["version"]},
        f"schedule-finalize-{RUN_KEY}",
    )
    require(finalized.status == 200, f"finalize failed: {finalized.status} {finalized.data}")

    final_state = request("GET", f"/api/leagues/{league_id}/schedule/state?season={SEASON}&week=1", commissioner)
    require(final_state.status == 200 and final_state.data["weekStatus"] == "finalized", f"week lineups not finalized: {final_state.data}")
    require({item["status"] for item in final_state.data["lineups"]} == {"finalized"}, "manager lineups not finalized")

    released_slot = request(
        "POST",
        f"/api/leagues/{league_id}/roster/schedule-player-{RUN_KEY}-2/slot",
        tokens[1],
        {"slot": "bench"},
    )
    require(released_slot.status == 200, f"finalized lineup did not release roster: {released_slot.status} {released_slot.data}")

    with psycopg.connect(DB_URL) as connection:
        with connection.cursor() as cursor:
            cursor.execute("SELECT version, weeks FROM schedule_states WHERE league_id = %s AND season = %s", (league_id, SEASON))
            schedule_version, weeks = cursor.fetchone()
            require(schedule_version == 7 and weeks == 3, f"unexpected schedule state: {(schedule_version, weeks)}")

            cursor.execute(
                "SELECT COUNT(*), COUNT(DISTINCT id), MIN(schedule_version), MAX(schedule_version) "
                "FROM league_matchups WHERE league_id = %s AND season = %s",
                (league_id, SEASON),
            )
            count, distinct_count, min_version, max_version = cursor.fetchone()
            require((count, distinct_count, min_version, max_version) == (6, 6, 1, 1), "matchup identities or versions changed")

            cursor.execute(
                "SELECT COUNT(*), COUNT(*) FILTER (WHERE status = 'finalized'), "
                "COUNT(*) FILTER (WHERE lock_reason = 'scoring') "
                "FROM lineup_week_states WHERE league_id = %s AND season = %s AND week = 1",
                (league_id, SEASON),
            )
            total, final_count, scoring_reason_count = cursor.fetchone()
            require((total, final_count, scoring_reason_count) == (4, 4, 4), "lineup finalization rows incorrect")

            cursor.execute(
                "SELECT status FROM schedule_week_states WHERE league_id = %s AND season = %s AND week = 1",
                (league_id, SEASON),
            )
            require(cursor.fetchone()[0] == "finalized", "schedule week was not finalized by scoring trigger")

            cursor.execute(
                "SELECT COUNT(*) FROM schedule_operations WHERE league_id = %s",
                (league_id,),
            )
            require(cursor.fetchone()[0] == 6, "unexpected schedule operation count")

    print("schedule lineup runtime contracts passed")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"schedule lineup runtime contract failed: {error}", file=sys.stderr)
        raise
