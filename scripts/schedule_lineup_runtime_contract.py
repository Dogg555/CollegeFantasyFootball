#!/usr/bin/env python3
"""Database-backed contracts for deterministic schedules and lineup locks."""
from __future__ import annotations

import concurrent.futures
import json
import os
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from typing import Any

import psycopg

BASE = os.getenv("CFF_API_BASE_URL", "http://127.0.0.1:8080").rstrip("/")
DB_URL = os.environ["DB_URL"]
ORIGIN = os.getenv("CFF_CONTRACT_ORIGIN", "https://frontend.example.test")
PASSWORD = os.getenv("CFF_CONTRACT_PASSWORD", "Schedule-Contract-Password-2026!")
RUN_KEY = os.getenv("CFF_SCHEDULE_RUN_KEY", str(time.time_ns()))
SEASON = 2026


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
    token: str = "",
    payload: Any | None = None,
    operation_key: str = "",
    timeout: int = 30,
) -> Response:
    headers = {"Accept": "application/json", "Origin": ORIGIN}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    if operation_key:
        headers["Idempotency-Key"] = operation_key
    body = None
    if payload is not None:
        body = json.dumps(payload).encode()
        headers["Content-Type"] = "application/json"
    request = urllib.request.Request(BASE + path, data=body, headers=headers, method=method)
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


def wait_for_api() -> None:
    last = "no response"
    for _ in range(90):
        try:
            response = call("GET", "/api/auth/status", timeout=3)
            if response.status == 200 and response.json().get("ready") is True:
                return
            last = f"HTTP {response.status}: {response.json()!r}"
        except Exception as exc:  # noqa: BLE001 - diagnostics
            last = str(exc)
        time.sleep(2)
    raise ContractFailure(f"API did not become ready: {last}")


def signup(email: str) -> str:
    body = expect(
        call("POST", "/api/auth/signup", payload={"email": email, "password": PASSWORD}),
        201,
        f"signup {email}",
    )
    token = str(body.get("token", ""))
    require(token.startswith("token-"), f"signup did not return bearer token for {email}: {body!r}")
    return token


def schedule_state(league_id: str, token: str) -> dict[str, Any]:
    return expect(
        call("GET", f"/api/leagues/{league_id}/schedule/state?season={SEASON}", token=token),
        200,
        "schedule state",
    )


def schedule_mutation(
    league_id: str,
    token: str,
    key: str,
    action: str,
    version: int,
    *,
    week: int = 1,
    extra: dict[str, Any] | None = None,
) -> Response:
    payload = {
        "action": action,
        "season": SEASON,
        "week": week,
        "expectedVersion": version,
    }
    payload.update(extra or {})
    return call(
        "POST",
        f"/api/leagues/{league_id}/schedule/transactions",
        token=token,
        operation_key=key,
        payload=payload,
    )


def scoring_mutation(
    league_id: str,
    token: str,
    key: str,
    action: str,
    version: int,
    week: int = 1,
) -> Response:
    return call(
        "POST",
        f"/api/leagues/{league_id}/scoring/transactions",
        token=token,
        operation_key=key,
        payload={
            "action": action,
            "season": SEASON,
            "week": week,
            "expectedVersion": version,
        },
    )


def configure_league(league_id: str, emails: list[str]) -> list[str]:
    player_ids = [f"schedule-player-{index}-{RUN_KEY}" for index in range(1, 5)]
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
                (
                    json.dumps({"qb": 1, "rb": 0, "wr": 0, "te": 0, "flex": 0, "bench": 0}),
                    json.dumps({
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
                    }),
                    league_id,
                ),
            )
            cursor.execute("DELETE FROM rosters WHERE league_id = %s", (league_id,))
            game_id = int(str(abs(hash(RUN_KEY)))[:12] or "1")
            cursor.execute(
                "INSERT INTO games (id, season, week, season_type, updated_at) VALUES (%s, %s, 1, 'regular', NOW()) "
                "ON CONFLICT (id) DO UPDATE SET season = EXCLUDED.season, week = EXCLUDED.week, updated_at = NOW()",
                (game_id, SEASON),
            )
            for index, (email, player_id) in enumerate(zip(emails, player_ids, strict=True), start=1):
                snapshot = {
                    "id": player_id,
                    "playerId": player_id,
                    "name": f"Schedule Player {index}",
                    "position": "QB",
                    "team": "Contract U",
                    "rosterSlot": "qb",
                }
                cursor.execute(
                    "INSERT INTO players (id, full_name, position, team, updated_at, raw) "
                    "VALUES (%s, %s, 'QB', 'Contract U', NOW(), %s::jsonb) "
                    "ON CONFLICT (id) DO UPDATE SET full_name = EXCLUDED.full_name, updated_at = NOW()",
                    (player_id, snapshot["name"], json.dumps(snapshot)),
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
                    "VALUES (%s, %s, 1, 'Contract U', 'Test', 'passing', 'passingYards', %s, %s, NOW()) "
                    "ON CONFLICT (player_id, season, week, category, stat_name, game_id) "
                    "DO UPDATE SET stat_value = EXCLUDED.stat_value, updated_at = NOW()",
                    (player_id, SEASON, index * 100, game_id),
                )
            cursor.execute(
                "SELECT COUNT(*) FROM league_members WHERE league_id = %s AND status = 'active'",
                (league_id,),
            )
            require(int(cursor.fetchone()[0]) == 4, "not all four managers became active")
        connection.commit()
    return player_ids


def database_snapshot(league_id: str) -> dict[str, Any]:
    with psycopg.connect(DB_URL) as connection:
        with connection.cursor() as cursor:
            cursor.execute(
                "SELECT season, weeks, current_week, version, schedule_hash "
                "FROM league_schedule_states WHERE league_id = %s",
                (league_id,),
            )
            season, weeks, current_week, version, schedule_hash = cursor.fetchone()
            cursor.execute(
                "SELECT week, lineup_version, status, lineup_hash, jsonb_array_length(lineup_snapshot) "
                "FROM league_week_controls WHERE league_id = %s AND season = %s ORDER BY week",
                (league_id, SEASON),
            )
            controls = cursor.fetchall()
            cursor.execute(
                "SELECT id, week, matchup_key, schedule_slot, schedule_version "
                "FROM league_matchups WHERE league_id = %s AND season = %s ORDER BY week, schedule_slot",
                (league_id, SEASON),
            )
            matchups = cursor.fetchall()
            cursor.execute(
                "SELECT COUNT(*) FROM schedule_operations WHERE league_id = %s",
                (league_id,),
            )
            operations = int(cursor.fetchone()[0])
            cursor.execute(
                "SELECT COUNT(*) FROM transactions WHERE league_id = %s AND transaction_type = 'Schedule'",
                (league_id,),
            )
            schedule_transactions = int(cursor.fetchone()[0])
    return {
        "season": int(season),
        "weeks": int(weeks),
        "currentWeek": int(current_week),
        "version": int(version),
        "scheduleHash": schedule_hash,
        "controls": controls,
        "matchups": matchups,
        "operations": operations,
        "scheduleTransactions": schedule_transactions,
    }


def main() -> None:
    wait_for_api()
    emails = [
        f"schedule-owner-{RUN_KEY}@example.test",
        f"schedule-member1-{RUN_KEY}@example.test",
        f"schedule-member2-{RUN_KEY}@example.test",
        f"schedule-member3-{RUN_KEY}@example.test",
    ]
    tokens = {email: signup(email) for email in emails}

    created = expect(
        call(
            "POST",
            "/api/leagues",
            token=tokens[emails[0]],
            operation_key=f"create-{RUN_KEY}",
            payload={
                "name": f"Schedule Contract {RUN_KEY}",
                "teams": 4,
                "scoring": "ppr",
                "draftType": "snake",
                "invitedEmails": emails[1:],
                "rosterRules": {"qb": 1, "rb": 0, "wr": 0, "te": 0, "flex": 0, "bench": 0},
            },
        ),
        201,
        "create schedule league",
    )
    league_id = str(created.get("id", ""))
    require(league_id, f"created league ID missing: {created!r}")
    player_ids = configure_league(league_id, emails)

    initial = schedule_state(league_id, tokens[emails[0]])
    require(initial.get("version") == 0, f"bad initial schedule state: {initial!r}")

    generate_keys = [f"generate-race-a-{RUN_KEY}", f"generate-race-b-{RUN_KEY}"]
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
        futures = [
            executor.submit(
                schedule_mutation,
                league_id,
                tokens[emails[0]],
                key,
                "generate",
                0,
                extra={"weeks": 6},
            )
            for key in generate_keys
        ]
        generate_responses = [future.result() for future in futures]
    statuses = sorted(response.status for response in generate_responses)
    require(statuses == [201, 409], f"generate race did not converge to one winner: {statuses!r}")
    winner_index = next(index for index, response in enumerate(generate_responses) if response.status == 201)
    winner = generate_responses[winner_index].json()
    winner_key = generate_keys[winner_index]
    conflict = next(response.json() for response in generate_responses if response.status == 409)
    require(conflict.get("code") == "schedule_state_conflict", f"wrong generation conflict: {conflict!r}")
    require(winner.get("version") == 1 and winner.get("generated") is True, f"bad generation winner: {winner!r}")
    matchup_ids = [item["id"] for item in winner.get("matchups", [])]
    require(len(matchup_ids) == 12 and len(set(matchup_ids)) == 12, f"unstable four-team schedule IDs: {matchup_ids!r}")

    replay = expect(
        schedule_mutation(league_id, tokens[emails[0]], winner_key, "generate", 0, extra={"weeks": 6}),
        200,
        "generate replay",
    )
    require(replay.get("idempotentReplay") is True and replay.get("version") == 1,
            f"generation replay advanced state: {replay!r}")

    unchanged = expect(
        schedule_mutation(
            league_id,
            tokens[emails[0]],
            f"generate-unchanged-{RUN_KEY}",
            "generate",
            1,
            extra={"weeks": 6},
        ),
        200,
        "unchanged generation",
    )
    require(unchanged.get("unchanged") is True and unchanged.get("version") == 1,
            f"identical schedule was rewritten: {unchanged!r}")
    require([item["id"] for item in unchanged.get("matchups", [])] == matchup_ids,
            "identical generation changed matchup identities")

    lock_keys = [f"lock-race-a-{RUN_KEY}", f"lock-race-b-{RUN_KEY}"]
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
        futures = [
            executor.submit(schedule_mutation, league_id, tokens[emails[0]], key, "lock", 1)
            for key in lock_keys
        ]
        lock_responses = [future.result() for future in futures]
    require(sorted(response.status for response in lock_responses) == [200, 409],
            f"lineup lock race did not converge: {[r.status for r in lock_responses]!r}")
    lock_winner_index = next(index for index, response in enumerate(lock_responses) if response.status == 200)
    lock_winner_key = lock_keys[lock_winner_index]
    locked = lock_responses[lock_winner_index].json()
    require(locked.get("version") == 2 and locked.get("changed") is True, f"lineup lock failed: {locked!r}")

    lock_replay = expect(
        schedule_mutation(league_id, tokens[emails[0]], lock_winner_key, "lock", 1),
        200,
        "lineup lock replay",
    )
    require(lock_replay.get("idempotentReplay") is True and lock_replay.get("version") == 2,
            f"lineup lock replay changed state: {lock_replay!r}")

    blocked_slot = call(
        "POST",
        f"/api/leagues/{league_id}/roster/{player_ids[0]}/slot",
        token=tokens[emails[0]],
        payload={"slot": "qb"},
    )
    require(blocked_slot.status == 409 and blocked_slot.json().get("code") == "lineup_locked",
            f"locked lineup allowed roster mutation: {blocked_slot.status} {blocked_slot.json()!r}")

    unlocked = expect(
        schedule_mutation(
            league_id,
            tokens[emails[0]],
            f"unlock-{RUN_KEY}",
            "unlock",
            2,
        ),
        200,
        "unlock before scoring",
    )
    require(unlocked.get("version") == 3 and unlocked.get("changed") is True, f"unlock failed: {unlocked!r}")

    allowed_slot = expect(
        call(
            "POST",
            f"/api/leagues/{league_id}/roster/{player_ids[0]}/slot",
            token=tokens[emails[0]],
            payload={"slot": "qb"},
        ),
        200,
        "roster mutation while lineup open",
    )
    require(isinstance(allowed_slot, list), f"roster mutation did not return authoritative roster: {allowed_slot!r}")

    past_deadline = (datetime.now(timezone.utc) - timedelta(minutes=1)).isoformat()
    deadline_locked = expect(
        schedule_mutation(
            league_id,
            tokens[emails[0]],
            f"deadline-{RUN_KEY}",
            "set_deadline",
            3,
            extra={"lineupDeadline": past_deadline},
        ),
        200,
        "past deadline auto lock",
    )
    require(deadline_locked.get("version") == 4, f"deadline did not advance schedule version: {deadline_locked!r}")
    week_one_control = next(item for item in deadline_locked.get("weekControls", []) if int(item["week"]) == 1)
    require(week_one_control.get("status") == "locked" and week_one_control.get("lockReason") == "deadline",
            f"past deadline did not capture and lock lineup: {week_one_control!r}")
    require(len(week_one_control.get("lineupSnapshot", [])) == 4,
            f"lineup snapshot did not capture all managers: {week_one_control!r}")

    scored = expect(
        scoring_mutation(league_id, tokens[emails[0]], f"score-{RUN_KEY}", "score", 0),
        200,
        "score locked week",
    )
    require(scored.get("version") == 1 and scored.get("status") == "scored", f"scoring failed: {scored!r}")

    unlock_after_score = schedule_mutation(
        league_id,
        tokens[emails[0]],
        f"unlock-after-score-{RUN_KEY}",
        "unlock",
        4,
    )
    require(unlock_after_score.status == 409 and unlock_after_score.json().get("code") == "lineup_unlock_forbidden",
            f"scored lineup was unlocked: {unlock_after_score.status} {unlock_after_score.json()!r}")

    regeneration = schedule_mutation(
        league_id,
        tokens[emails[0]],
        f"regenerate-after-score-{RUN_KEY}",
        "generate",
        4,
        extra={"weeks": 6},
    )
    require(regeneration.status == 409 and regeneration.json().get("code") == "schedule_locked",
            f"scored schedule was regenerated: {regeneration.status} {regeneration.json()!r}")

    finalized = expect(
        scoring_mutation(league_id, tokens[emails[0]], f"finalize-{RUN_KEY}", "finalize", 1),
        200,
        "finalize locked week",
    )
    require(finalized.get("status") == "final", f"week did not finalize: {finalized!r}")

    after_final = schedule_state(league_id, tokens[emails[0]])
    require(after_final.get("version") == 5 and after_final.get("currentWeek") == 2,
            f"finalization did not version and advance active week: {after_final!r}")
    final_control = next(item for item in after_final.get("weekControls", []) if int(item["week"]) == 1)
    require(final_control.get("status") == "finalized", f"week control not finalized: {final_control!r}")
    require([item["id"] for item in after_final.get("matchups", [])] == matchup_ids,
            "scoring or finalization changed stable matchup identities")

    post_final_slot = expect(
        call(
            "POST",
            f"/api/leagues/{league_id}/roster/{player_ids[0]}/slot",
            token=tokens[emails[0]],
            payload={"slot": "qb"},
        ),
        200,
        "next-week roster mutation",
    )
    require(isinstance(post_final_slot, list), "active week did not unlock after finalization")

    database = database_snapshot(league_id)
    require(database["season"] == SEASON and database["weeks"] == 6, f"database schedule state wrong: {database!r}")
    require(database["currentWeek"] == 2 and database["version"] == 5, f"database active week/version wrong: {database!r}")
    require(len(database["matchups"]) == 12, f"database schedule size wrong: {database!r}")
    require(len({row[0] for row in database["matchups"]}) == 12, "database matchup IDs are not unique")
    require(all(int(row[4]) == 1 for row in database["matchups"]), "matchups do not retain generation version")
    require(database["controls"][0][2] == "finalized", f"week one control not finalized in DB: {database!r}")
    require(int(database["controls"][0][1]) == 2, f"lineup version did not record both locks: {database!r}")
    require(int(database["controls"][0][4]) == 4, f"lineup snapshot missing players: {database!r}")
    require(database["operations"] >= 5, f"schedule operations were not retained: {database!r}")
    require(database["scheduleTransactions"] == 1, f"identical generation logged duplicate transactions: {database!r}")

    print("schedule lineup runtime contracts passed")


if __name__ == "__main__":
    main()
