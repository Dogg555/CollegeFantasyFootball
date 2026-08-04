#!/usr/bin/env python3
"""PostgreSQL contracts for leased player-stat ingestion and corrections."""
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
ADMIN_TOKEN = os.environ["CFF_ADMIN_API_TOKEN"]
RUN_KEY = os.getenv("CFF_STAT_RUN_KEY", str(time.time_ns()))
SEASON = 2026
WEEK = 1


class ContractFailure(RuntimeError):
    pass


@dataclass(frozen=True)
class Response:
    status: int
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


def call(method: str, path: str, *, payload: Any | None = None, operation_key: str = "") -> Response:
    headers = {
        "Accept": "application/json",
        "Origin": ORIGIN,
        "Authorization": f"Bearer {ADMIN_TOKEN}",
    }
    body = None
    if payload is not None:
        body = json.dumps(payload).encode()
        headers["Content-Type"] = "application/json"
    if operation_key:
        headers["Idempotency-Key"] = operation_key
    request = urllib.request.Request(BASE + path, data=body, headers=headers, method=method)
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            return Response(response.status, response.read())
    except urllib.error.HTTPError as error:
        return Response(error.code, error.read())
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
            response = call("GET", "/api/admin/ingest/cfbd/stats/status?season=2026&week=1")
            if response.status == 200:
                return
            last = f"HTTP {response.status}: {response.json()!r}"
        except Exception as exc:  # noqa: BLE001
            last = str(exc)
        time.sleep(2)
    raise ContractFailure(f"API did not become ready: {last}")


def status() -> dict[str, Any]:
    return expect(
        call("GET", f"/api/admin/ingest/cfbd/stats/status?season={SEASON}&week={WEEK}"),
        200,
        "stat status",
    )


def mutate(action: str, key: str, expected_version: int, **extra: Any) -> Response:
    payload = {
        "action": action,
        "season": SEASON,
        "week": WEEK,
        "expectedVersion": expected_version,
        **extra,
    }
    return call(
        "POST",
        "/api/admin/ingest/cfbd/stats/transactions",
        payload=payload,
        operation_key=key,
    )


def seed_database() -> tuple[str, int, str, str]:
    player_id = f"stat-player-{RUN_KEY}"
    game_id = int(str(abs(hash(RUN_KEY)))[:12] or "1001")
    scored_league = f"stat-scored-{RUN_KEY}"
    final_league = f"stat-final-{RUN_KEY}"
    owner = f"stat-owner-{RUN_KEY}@example.test"
    player = {
        "id": player_id,
        "playerId": player_id,
        "name": "Stat Contract Quarterback",
        "position": "QB",
        "team": "Contract U",
        "rosterSlot": "qb",
    }
    with psycopg.connect(DB_URL) as connection:
        with connection.cursor() as cursor:
            cursor.execute(
                "INSERT INTO players (id, full_name, position, team, conference, season, active, updated_at, raw) "
                "VALUES (%s, 'Stat Contract Quarterback', 'QB', 'Contract U', 'Test', %s, TRUE, NOW(), %s::jsonb) "
                "ON CONFLICT (id) DO UPDATE SET active = TRUE, updated_at = NOW()",
                (player_id, SEASON, json.dumps(player)),
            )
            cursor.execute(
                "INSERT INTO games (id, season, week, season_type, home_team, away_team, updated_at) "
                "VALUES (%s, %s, %s, 'regular', 'Contract U', 'Other U', NOW()) "
                "ON CONFLICT (id) DO UPDATE SET season = EXCLUDED.season, week = EXCLUDED.week, updated_at = NOW()",
                (game_id, SEASON, WEEK),
            )
            for league_id, scoring_status in ((scored_league, "scored"), (final_league, "final")):
                cursor.execute(
                    "INSERT INTO leagues (id, account_email, name, team_count, scoring, draft_type, notes, created_at, updated_at) "
                    "VALUES (%s, %s, %s, 4, 'ppr', 'snake', '', NOW(), NOW())",
                    (league_id, owner, league_id),
                )
                cursor.execute(
                    "INSERT INTO league_members (league_id, email, role, status, joined_at, created_at, updated_at) "
                    "VALUES (%s, %s, 'commissioner', 'active', NOW(), NOW(), NOW())",
                    (league_id, owner),
                )
                cursor.execute(
                    "INSERT INTO rosters (league_id, manager_email, player_id, player_snapshot, roster_slot, acquired_via, acquired_at) "
                    "VALUES (%s, %s, %s, %s::jsonb, 'qb', 'draft', NOW())",
                    (league_id, owner, player_id, json.dumps(player)),
                )
                cursor.execute(
                    "INSERT INTO scoring_states (league_id, version, standings_version, updated_at) "
                    "VALUES (%s, 1, 0, NOW()) ON CONFLICT (league_id) DO NOTHING",
                    (league_id,),
                )
                cursor.execute(
                    "INSERT INTO scoring_week_states "
                    "(league_id, season, week, version, status, input_hash, scored_at, finalized_at, updated_at) "
                    "VALUES (%s, %s, %s, 1, %s, 'contract-hash', NOW(), "
                    "CASE WHEN %s = 'final' THEN NOW() ELSE NULL END, NOW())",
                    (league_id, SEASON, WEEK, scoring_status, scoring_status),
                )
        connection.commit()
    return player_id, game_id, scored_league, final_league


def record(player_id: str, game_id: int, value: float) -> dict[str, Any]:
    return {
        "playerId": player_id,
        "category": "passing",
        "statName": "passingYards",
        "statValue": value,
        "gameId": game_id,
        "team": "Contract U",
        "conference": "Test",
    }


def db_snapshot(player_id: str, game_id: int) -> dict[str, Any]:
    with psycopg.connect(DB_URL) as connection:
        with connection.cursor() as cursor:
            cursor.execute(
                "SELECT stat_value, source_revision, source_hash, corrected_at IS NOT NULL "
                "FROM player_stats WHERE player_id = %s AND season = %s AND week = %s "
                "AND category = 'passing' AND stat_name = 'passingyards' AND game_id = %s",
                (player_id, SEASON, WEEK, game_id),
            )
            stat = cursor.fetchone()
            cursor.execute(
                "SELECT change_type, source_revision, previous_value, new_value "
                "FROM player_stat_revisions WHERE player_id = %s AND season = %s AND week = %s "
                "ORDER BY id",
                (player_id, SEASON, WEEK),
            )
            revisions = cursor.fetchall()
            cursor.execute(
                "SELECT league_id, status, reason, source_revision, player_ids "
                "FROM scoring_recalculation_queue WHERE season = %s AND week = %s ORDER BY league_id",
                (SEASON, WEEK),
            )
            queue = cursor.fetchall()
            cursor.execute(
                "SELECT version, source_revision, status, active_run_id, last_success_at, next_retry_at "
                "FROM stat_ingestion_states WHERE season = %s AND week = %s",
                (SEASON, WEEK),
            )
            state = cursor.fetchone()
            cursor.execute(
                "SELECT COUNT(*), COUNT(*) FILTER (WHERE status = 'success'), "
                "COUNT(*) FILTER (WHERE status = 'retry_wait'), COUNT(*) FILTER (WHERE status = 'abandoned') "
                "FROM ingestion_runs WHERE resource = 'player_stats' AND season = %s AND week = %s",
                (SEASON, WEEK),
            )
            runs = cursor.fetchone()
            cursor.execute(
                "SELECT COUNT(*) FROM stat_ingestion_operations WHERE season = %s AND week = %s",
                (SEASON, WEEK),
            )
            operations = int(cursor.fetchone()[0])
    return {
        "stat": stat,
        "revisions": revisions,
        "queue": queue,
        "state": state,
        "runs": runs,
        "operations": operations,
    }


def main() -> None:
    wait_for_api()
    player_id, game_id, scored_league, final_league = seed_database()

    initial = status()
    require(initial["version"] == 0 and initial["sourceRevision"] == 0, f"bad initial state: {initial!r}")

    start_keys = [f"start-a-{RUN_KEY}", f"start-b-{RUN_KEY}"]
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
        futures = [executor.submit(mutate, "start", key, 0, ownerId=f"worker-{key}") for key in start_keys]
        responses = [future.result() for future in futures]
    require(sorted(response.status for response in responses) == [201, 409],
            f"run ownership race did not converge: {[r.status for r in responses]!r}")
    winner_index = next(index for index, response in enumerate(responses) if response.status == 201)
    winner_key = start_keys[winner_index]
    started = responses[winner_index].json()
    run_id = int(started["runId"])
    owner_id = started["ownerId"]
    require(started["version"] == 1 and started["status"] == "running", f"bad run start: {started!r}")
    loser = next(response.json() for response in responses if response.status == 409)
    require(loser.get("code") in {"stat_ingestion_state_conflict", "ingestion_run_active"}, f"wrong race conflict: {loser!r}")

    replay = expect(mutate("start", winner_key, 0, ownerId=owner_id), 200, "start replay")
    require(replay.get("idempotentReplay") is True and replay["runId"] == run_id, f"start replay changed run: {replay!r}")

    heartbeat = expect(
        mutate("heartbeat", f"heartbeat-{RUN_KEY}", 1, runId=run_id, ownerId=owner_id, leaseSeconds=300),
        200,
        "heartbeat",
    )
    require(heartbeat.get("heartbeat") is True and heartbeat["version"] == 1, f"heartbeat changed source state: {heartbeat!r}")

    first_record = record(player_id, game_id, 250.0)
    applied = expect(
        mutate(
            "apply",
            f"apply-first-{RUN_KEY}",
            1,
            runId=run_id,
            ownerId=owner_id,
            apiCalls=1,
            records=[first_record, dict(first_record)],
        ),
        200,
        "initial stat apply",
    )
    require(applied["version"] == 2 and applied["sourceRevision"] == 1, f"bad initial apply: {applied!r}")
    require(applied["inserted"] == 1 and applied["duplicateRecords"] == 1, f"duplicate was not suppressed: {applied!r}")
    queue = {item["leagueId"]: item for item in applied["recalculationQueue"]}
    require(queue[scored_league]["status"] == "pending", f"scored league not queued: {queue!r}")
    require(queue[final_league]["status"] == "blocked_final", f"final league was not protected: {queue!r}")

    apply_replay = expect(
        mutate(
            "apply",
            f"apply-first-{RUN_KEY}",
            1,
            runId=run_id,
            ownerId=owner_id,
            records=[first_record, dict(first_record)],
        ),
        200,
        "apply replay",
    )
    require(apply_replay.get("idempotentReplay") is True and apply_replay["sourceRevision"] == 1,
            f"apply replay created another revision: {apply_replay!r}")

    second = expect(mutate("start", f"start-second-{RUN_KEY}", 2, ownerId="worker-second"), 201, "second run")
    unchanged = expect(
        mutate(
            "apply",
            f"apply-unchanged-{RUN_KEY}",
            3,
            runId=int(second["runId"]),
            ownerId="worker-second",
            records=[first_record],
        ),
        200,
        "unchanged apply",
    )
    require(unchanged["version"] == 4 and unchanged["sourceRevision"] == 1 and unchanged["unchanged"] == 1,
            f"unchanged source advanced revision: {unchanged!r}")

    third = expect(mutate("start", f"start-third-{RUN_KEY}", 4, ownerId="worker-third"), 201, "third run")
    corrected_record = record(player_id, game_id, 300.0)
    corrected = expect(
        mutate(
            "apply",
            f"apply-correction-{RUN_KEY}",
            5,
            runId=int(third["runId"]),
            ownerId="worker-third",
            records=[corrected_record],
        ),
        200,
        "correction apply",
    )
    require(corrected["version"] == 6 and corrected["sourceRevision"] == 2 and corrected["corrected"] == 1,
            f"correction did not create one source revision: {corrected!r}")

    fourth = expect(mutate("start", f"start-fourth-{RUN_KEY}", 6, ownerId="worker-fourth"), 201, "fourth run")
    failed = expect(
        mutate(
            "fail",
            f"fail-429-{RUN_KEY}",
            7,
            runId=int(fourth["runId"]),
            ownerId="worker-fourth",
            providerStatus=429,
            retryAfterSeconds=120,
            error="provider quota exhausted",
        ),
        200,
        "429 failure",
    )
    require(failed["version"] == 8 and failed["status"] == "retry_wait", f"429 did not enter retry wait: {failed!r}")
    require(failed["retryable"] is True and failed["retryDelaySeconds"] == 120, f"Retry-After was not honored: {failed!r}")

    blocked = mutate("start", f"start-blocked-{RUN_KEY}", 8, ownerId="worker-blocked")
    require(blocked.status == 409 and blocked.json().get("code") == "ingestion_backoff_active",
            f"backoff allowed another provider call: {blocked.status} {blocked.json()!r}")

    recovered = expect(mutate("recover", f"recover-backoff-{RUN_KEY}", 8), 200, "backoff recovery")
    require(recovered["version"] == 9 and recovered["status"] == "idle", f"backoff recovery failed: {recovered!r}")

    fifth = expect(mutate("start", f"start-fifth-{RUN_KEY}", 9, ownerId="worker-fifth", leaseSeconds=30), 201, "fifth run")
    with psycopg.connect(DB_URL) as connection:
        with connection.cursor() as cursor:
            cursor.execute("UPDATE ingestion_runs SET lease_expires_at = NOW() - INTERVAL '1 second' WHERE id = %s", (int(fifth["runId"]),))
        connection.commit()
    lease_recovery = expect(mutate("recover", f"recover-lease-{RUN_KEY}", 10), 200, "expired lease recovery")
    require(lease_recovery["status"] == "idle" and lease_recovery["version"] == 12,
            f"expired lease was not recovered deterministically: {lease_recovery!r}")

    with psycopg.connect(DB_URL) as connection:
        with connection.cursor() as cursor:
            cursor.execute(
                "UPDATE stat_ingestion_states SET status = 'fresh', last_success_at = NOW() - INTERVAL '1 hour', "
                "stale_after_seconds = 60 WHERE season = %s AND week = %s",
                (SEASON, WEEK),
            )
        connection.commit()
    stale = status()
    require(stale["status"] == "stale" and stale["fresh"] is False, f"stale source was not visible: {stale!r}")

    database = db_snapshot(player_id, game_id)
    require(float(database["stat"][0]) == 300.0 and int(database["stat"][1]) == 2,
            f"corrected stat not authoritative: {database!r}")
    require(database["stat"][3] is True, f"corrected_at was not recorded: {database!r}")
    require([row[0] for row in database["revisions"]] == ["inserted", "corrected"],
            f"revision audit trail is wrong: {database!r}")
    queues = {row[0]: row for row in database["queue"]}
    require(queues[scored_league][1] == "pending" and queues[scored_league][3] == 2,
            f"scored queue is wrong: {database!r}")
    require(queues[final_league][1] == "blocked_final" and queues[final_league][2] == "final_week_immutable",
            f"final week correction was not blocked: {database!r}")
    require(database["runs"][1] == 3 and database["runs"][2] == 1 and database["runs"][3] == 1,
            f"run terminal states are wrong: {database!r}")
    require(database["operations"] >= 10, f"operation replay ledger is incomplete: {database!r}")

    print("stat ingestion runtime contracts passed")


if __name__ == "__main__":
    main()
