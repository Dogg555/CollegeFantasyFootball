#!/usr/bin/env python3
"""Database-backed contracts for scoring snapshots, finalization, and standings."""
from __future__ import annotations

import concurrent.futures
import json
import os
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from typing import Any

import psycopg

BASE = os.getenv("CFF_API_BASE_URL", "http://127.0.0.1:8080").rstrip("/")
DB_URL = os.environ["DB_URL"]
ORIGIN = os.getenv("CFF_CONTRACT_ORIGIN", "https://frontend.example.test")
PASSWORD = os.getenv("CFF_CONTRACT_PASSWORD", "Scoring-Contract-Password-2026!")
RUN_KEY = os.getenv("CFF_SCORING_RUN_KEY", str(time.time_ns()))
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


def state(league_id: str, token: str, week: int = 1) -> dict[str, Any]:
    return expect(
        call("GET", f"/api/leagues/{league_id}/scoring/state?season={SEASON}&week={week}", token=token),
        200,
        f"scoring state week {week}",
    )


def mutate(
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
    player_ids = [f"score-player-{index}-{RUN_KEY}" for index in range(1, 5)]
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
                    "name": f"Scoring Player {index}",
                    "position": "QB",
                    "team": "Contract U",
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


def update_stats(player_ids: list[str]) -> None:
    with psycopg.connect(DB_URL) as connection:
        with connection.cursor() as cursor:
            for index, player_id in enumerate(player_ids, start=1):
                cursor.execute(
                    "UPDATE player_stats SET stat_value = %s, updated_at = NOW() "
                    "WHERE player_id = %s AND season = %s AND week = 1",
                    (index * 100 + 25, player_id, SEASON),
                )
        connection.commit()


def database_snapshot(league_id: str) -> dict[str, Any]:
    with psycopg.connect(DB_URL) as connection:
        with connection.cursor() as cursor:
            cursor.execute(
                "SELECT version, standings_version FROM scoring_states WHERE league_id = %s",
                (league_id,),
            )
            global_version, standings_version = cursor.fetchone()
            cursor.execute(
                "SELECT version, status, input_hash, player_scores_snapshot, matchup_snapshot "
                "FROM scoring_week_states WHERE league_id = %s AND season = %s AND week = 1",
                (league_id, SEASON),
            )
            week_version, week_status, input_hash, scores, matchups = cursor.fetchone()
            cursor.execute(
                "SELECT manager_email, rank, wins, losses, ties, games_played, points_for, points_against, win_pct "
                "FROM league_standings WHERE league_id = %s AND season = %s ORDER BY rank, manager_email",
                (league_id, SEASON),
            )
            standings = cursor.fetchall()
            cursor.execute(
                "SELECT COUNT(*) FROM transactions WHERE league_id = %s AND transaction_type = 'Scoring Finalized'",
                (league_id,),
            )
            final_transactions = int(cursor.fetchone()[0])
            cursor.execute(
                "SELECT COUNT(*) FROM scoring_operations WHERE league_id = %s AND operation_type = 'finalize'",
                (league_id,),
            )
            final_operations = int(cursor.fetchone()[0])
    return {
        "globalVersion": int(global_version),
        "standingsVersion": int(standings_version),
        "weekVersion": int(week_version),
        "weekStatus": week_status,
        "inputHash": input_hash,
        "scores": scores,
        "matchups": matchups,
        "standings": standings,
        "finalTransactions": final_transactions,
        "finalOperations": final_operations,
    }


def main() -> None:
    wait_for_api()
    emails = [
        f"scoring-owner-{RUN_KEY}@example.test",
        f"scoring-member1-{RUN_KEY}@example.test",
        f"scoring-member2-{RUN_KEY}@example.test",
        f"scoring-member3-{RUN_KEY}@example.test",
    ]
    tokens = {email: signup(email) for email in emails}

    created = expect(
        call(
            "POST",
            "/api/leagues",
            token=tokens[emails[0]],
            operation_key=f"create-{RUN_KEY}",
            payload={
                "name": f"Scoring Contract {RUN_KEY}",
                "teams": 4,
                "scoring": "ppr",
                "draftType": "snake",
                "invitedEmails": emails[1:],
                "rosterRules": {"qb": 1, "rb": 0, "wr": 0, "te": 0, "flex": 0, "bench": 0},
            },
        ),
        201,
        "create scoring league",
    )
    league_id = str(created.get("id", ""))
    require(league_id, f"created league ID missing: {created!r}")
    player_ids = configure_league(league_id, emails)

    initial = state(league_id, tokens[emails[0]])
    require(initial.get("version") == 0 and initial.get("status") == "unscored", f"bad initial state: {initial!r}")

    score_keys = [f"score-race-a-{RUN_KEY}", f"score-race-b-{RUN_KEY}"]
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
        futures = [
            executor.submit(mutate, league_id, tokens[emails[0]], key, "score", 0)
            for key in score_keys
        ]
        score_responses = [future.result() for future in futures]
    statuses = sorted(response.status for response in score_responses)
    require(statuses == [200, 409], f"score race did not converge to one winner: {statuses!r}")
    winner_index = next(index for index, response in enumerate(score_responses) if response.status == 200)
    score_winner = score_responses[winner_index].json()
    score_winner_key = score_keys[winner_index]
    conflict = next(response.json() for response in score_responses if response.status == 409)
    require(conflict.get("code") == "scoring_state_conflict", f"wrong score conflict: {conflict!r}")
    require(score_winner.get("version") == 1, f"first score did not advance week version once: {score_winner!r}")

    replay_score = expect(
        mutate(league_id, tokens[emails[0]], score_winner_key, "score", 0),
        200,
        "score winner replay",
    )
    require(replay_score.get("idempotentReplay") is True, f"score replay not identified: {replay_score!r}")
    require(replay_score.get("version") == 1, "score replay advanced version")

    update_stats(player_ids)
    rescored = expect(
        mutate(league_id, tokens[emails[0]], f"rescore-{RUN_KEY}", "score", 1),
        200,
        "score after stats correction",
    )
    require(rescored.get("version") == 2, f"corrected scoring did not advance version: {rescored!r}")
    final_score_points = sorted(round(float(item["fantasyPoints"]), 6) for item in rescored.get("scores", []))
    require(final_score_points == [5.0, 9.0, 13.0, 17.0], f"unexpected corrected scores: {final_score_points!r}")

    finalize_keys = [f"finalize-race-a-{RUN_KEY}", f"finalize-race-b-{RUN_KEY}"]
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
        futures = [
            executor.submit(mutate, league_id, tokens[emails[0]], key, "finalize", 2)
            for key in finalize_keys
        ]
        finalize_responses = [future.result() for future in futures]
    require([response.status for response in finalize_responses] == [200, 200],
            f"concurrent finalize requests were not idempotent: {[r.status for r in finalize_responses]!r}")
    finalize_bodies = [response.json() for response in finalize_responses]
    require(sum(1 for body in finalize_bodies if body.get("alreadyFinal") is True) == 1,
            f"one concurrent finalize should observe the final state: {finalize_bodies!r}")
    require(all(body.get("status") == "final" for body in finalize_bodies), "finalize did not return final state")
    require(all(body.get("version") == 3 for body in finalize_bodies), "finalize advanced week version more than once")

    replay_final = expect(
        mutate(league_id, tokens[emails[0]], finalize_keys[0], "finalize", 2),
        200,
        "finalize replay",
    )
    require(replay_final.get("version") == 3, "finalize replay advanced version")

    immutable_before = state(league_id, tokens[emails[0]])
    with psycopg.connect(DB_URL) as connection:
        with connection.cursor() as cursor:
            cursor.execute(
                "UPDATE player_stats SET stat_value = stat_value + 1000, updated_at = NOW() "
                "WHERE season = %s AND week = 1 AND player_id = ANY(%s)",
                (SEASON, player_ids),
            )
        connection.commit()
    immutable_after = state(league_id, tokens[emails[0]])
    require(immutable_after.get("inputHash") == immutable_before.get("inputHash"), "final snapshot hash changed after stat mutation")
    require(immutable_after.get("scores") == immutable_before.get("scores"), "final player scores changed after stat mutation")
    require(immutable_after.get("matchups") == immutable_before.get("matchups"), "final matchups changed after stat mutation")

    post_final_score = mutate(
        league_id,
        tokens[emails[0]],
        f"post-final-score-{RUN_KEY}",
        "score",
        3,
    )
    require(post_final_score.status == 409 and post_final_score.json().get("code") == "week_finalized",
            f"final week accepted rescoring: {post_final_score.status} {post_final_score.json()!r}")

    week_two_finalize = mutate(
        league_id,
        tokens[emails[0]],
        f"week-two-finalize-{RUN_KEY}",
        "finalize",
        0,
        week=2,
    )
    require(week_two_finalize.status == 409 and week_two_finalize.json().get("code") == "week_not_scored",
            f"unscored week finalized: {week_two_finalize.status} {week_two_finalize.json()!r}")

    standings_response = expect(
        call("GET", f"/api/leagues/{league_id}/standings?season={SEASON}", token=tokens[emails[0]]),
        200,
        "authoritative standings",
    )
    standings = standings_response.get("standings", [])
    require(len(standings) == 4, f"standings did not include four managers: {standings!r}")
    require(sum(int(row.get("gamesPlayed", 0)) for row in standings) == 4,
            f"standings counted the finalized week incorrectly: {standings!r}")

    database = database_snapshot(league_id)
    require(database["weekStatus"] == "final" and database["weekVersion"] == 3,
            f"database week state wrong: {database!r}")
    require(database["standingsVersion"] == 1, f"standings rebuilt more than once: {database!r}")
    require(database["finalTransactions"] == 1, f"finalization transaction logged more than once: {database!r}")
    require(database["finalOperations"] >= 2, f"concurrent finalization replay operations not recorded: {database!r}")
    require(len(database["standings"]) == 4, f"materialized standings missing managers: {database!r}")
    require([row[0] for row in database["standings"]] == [row["managerEmail"] for row in standings],
            "API standings differ from materialized database standings")

    print("scoring lifecycle runtime contracts passed")


if __name__ == "__main__":
    main()
