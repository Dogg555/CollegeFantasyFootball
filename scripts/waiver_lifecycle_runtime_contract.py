#!/usr/bin/env python3
"""Database-backed contracts for deterministic, replay-safe waiver processing."""
from __future__ import annotations

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
PASSWORD = os.getenv("CFF_CONTRACT_PASSWORD", "Waiver-Contract-Password-2026!")
RUN_KEY = os.getenv("CFF_WAIVER_RUN_KEY", str(time.time_ns()))
PROCESSING_PERIOD = f"waiver-period-{RUN_KEY}"
OPEN_DEADLINE = "2999-01-01T00:00"
CLOSED_DEADLINE = "2000-01-01T00:00"


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
    timeout: int = 20,
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
        except Exception as exc:  # noqa: BLE001 - contract diagnostics
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


def transaction(
    league_id: str,
    token: str,
    key: str,
    action: str,
    version: int,
    **details: Any,
) -> Response:
    return call(
        "POST",
        f"/api/leagues/{league_id}/waivers/transactions",
        token=token,
        operation_key=key,
        payload={"action": action, "expectedVersion": version, **details},
    )


def spoofed_player(player_id: str, name: str, position: str = "WR") -> dict[str, Any]:
    return {
        "id": player_id,
        "playerId": player_id,
        "name": name,
        "position": position,
        "team": "Spoofed U",
        "projection": 999.0,
    }


def canonical_players() -> list[tuple[str, str, str]]:
    return [
        (f"player-x-{RUN_KEY}", "Player X", "Contract X"),
        (f"player-y-{RUN_KEY}", "Player Y", "Contract Y"),
        (f"player-z-{RUN_KEY}", "Player Z", "Contract Z"),
        (f"player-cancel-{RUN_KEY}", "Cancel Player", "Contract Cancel"),
    ]


def configure_league(league_id: str, emails: list[str]) -> None:
    with psycopg.connect(DB_URL) as connection:
        with connection.cursor() as cursor:
            cursor.execute(
                "UPDATE league_members SET status = 'active', joined_at = COALESCE(joined_at, NOW()), "
                "updated_at = NOW() WHERE league_id = %s",
                (league_id,),
            )
            cursor.execute(
                "UPDATE leagues SET waiver_rules = %s::jsonb, updated_at = NOW() WHERE id = %s",
                (
                    json.dumps(
                        {
                            "mode": "waivers",
                            "claimDeadline": OPEN_DEADLINE,
                            "processingPeriod": PROCESSING_PERIOD,
                            "freeAgencyLocked": True,
                        }
                    ),
                    league_id,
                ),
            )
            cursor.execute("DELETE FROM waiver_priorities WHERE league_id = %s", (league_id,))
            for priority, email in enumerate(emails, start=1):
                cursor.execute(
                    "INSERT INTO waiver_priorities (league_id, manager_email, priority, updated_at) "
                    "VALUES (%s, %s, %s, NOW())",
                    (league_id, email, priority),
                )
            for player_id, name, team in canonical_players():
                cursor.execute(
                    "INSERT INTO players "
                    "(id, full_name, position, team, conference, year, active, season, updated_at) "
                    "VALUES (%s, %s, 'WR', %s, 'Contract Conference', 'JR', TRUE, 2026, NOW()) "
                    "ON CONFLICT (id) DO UPDATE SET full_name = EXCLUDED.full_name, "
                    "position = EXCLUDED.position, team = EXCLUDED.team, conference = EXCLUDED.conference, "
                    "year = EXCLUDED.year, active = TRUE, season = 2026, updated_at = NOW()",
                    (player_id, name, team),
                )
            cursor.execute(
                "INSERT INTO roster_states (league_id, manager_email, version, updated_at) "
                "SELECT %s, email, 0, NOW() FROM league_members WHERE league_id = %s "
                "ON CONFLICT (league_id, manager_email) DO NOTHING",
                (league_id, league_id),
            )
            cursor.execute(
                "INSERT INTO waiver_states (league_id, version, last_processing_period, updated_at) "
                "VALUES (%s, 0, '', NOW()) "
                "ON CONFLICT (league_id) DO UPDATE SET version = 0, last_processing_period = '', "
                "last_processed_at = NULL, last_processing_run_id = NULL, updated_at = NOW()",
                (league_id,),
            )
            cursor.execute(
                "SELECT COUNT(*) FROM league_members WHERE league_id = %s AND status = 'active'",
                (league_id,),
            )
            require(int(cursor.fetchone()[0]) == len(emails), "not all invited managers became active")
        connection.commit()


def close_processing_period(league_id: str) -> None:
    with psycopg.connect(DB_URL) as connection:
        with connection.cursor() as cursor:
            cursor.execute(
                "UPDATE leagues SET waiver_rules = jsonb_set(waiver_rules, '{claimDeadline}', to_jsonb(%s::text)), "
                "updated_at = NOW() WHERE id = %s",
                (CLOSED_DEADLINE, league_id),
            )
        connection.commit()


def db_player_owner(league_id: str, player_id: str) -> str:
    with psycopg.connect(DB_URL) as connection:
        with connection.cursor() as cursor:
            cursor.execute(
                "SELECT lower(manager_email) FROM rosters WHERE league_id = %s AND player_id = %s",
                (league_id, player_id),
            )
            rows = cursor.fetchall()
            require(len(rows) <= 1, f"player {player_id} has duplicate owners: {rows!r}")
            return str(rows[0][0]) if rows else ""


def db_roster_snapshot_name(league_id: str, player_id: str) -> str:
    with psycopg.connect(DB_URL) as connection:
        with connection.cursor() as cursor:
            cursor.execute(
                "SELECT COALESCE(player_snapshot->>'name', '') FROM rosters "
                "WHERE league_id = %s AND player_id = %s",
                (league_id, player_id),
            )
            row = cursor.fetchone()
            return str(row[0]) if row else ""


def main() -> None:
    wait_for_api()
    emails = [
        f"waiver-owner-{RUN_KEY}@example.test",
        f"waiver-member1-{RUN_KEY}@example.test",
        f"waiver-member2-{RUN_KEY}@example.test",
        f"waiver-member3-{RUN_KEY}@example.test",
    ]
    tokens = {email: signup(email) for email in emails}

    created = expect(
        call(
            "POST",
            "/api/leagues",
            token=tokens[emails[0]],
            operation_key=f"create-{RUN_KEY}",
            payload={
                "name": f"Waiver Contract {RUN_KEY}",
                "teams": 4,
                "scoring": "ppr",
                "draftType": "snake",
                "invitedEmails": emails[1:],
                "rosterRules": {"qb": 1, "rb": 2, "wr": 2, "te": 1, "flex": 2, "bench": 6},
            },
        ),
        201,
        "create waiver league",
    )
    league_id = str(created.get("id", ""))
    require(league_id, f"created league ID missing: {created!r}")
    configure_league(league_id, emails)

    state = expect(
        call("GET", f"/api/leagues/{league_id}/waivers/state", token=tokens[emails[0]]),
        200,
        "initial waiver state",
    )
    require(state.get("version") == 0, f"initial waiver version wrong: {state!r}")
    require(state.get("waiverModeActive") is True, f"waiver mode not active: {state!r}")
    require(state.get("deadlinePassed") is False, f"open deadline not recognized: {state!r}")
    require(state.get("claimsMutable") is True, f"open-period claim controls are disabled: {state!r}")
    require(state.get("processingPeriod") == PROCESSING_PERIOD, f"processing period missing: {state!r}")

    version = int(state["version"])
    unknown = expect(
        transaction(
            league_id,
            tokens[emails[0]],
            f"unknown-player-{RUN_KEY}",
            "create",
            version,
            addPlayerId=f"unknown-{RUN_KEY}",
        ),
        409,
        "unknown player claim",
    )
    require(unknown.get("code") == "player_inactive", f"unknown player was not rejected authoritatively: {unknown!r}")

    owner_y = expect(
        transaction(
            league_id,
            tokens[emails[0]],
            f"owner-y-{RUN_KEY}",
            "create",
            version,
            addPlayer=spoofed_player(f"player-y-{RUN_KEY}", "Spoofed Player Y"),
        ),
        200,
        "owner claim Y",
    )
    owner_y_id = str(owner_y["claimId"])
    owner_y_claim = next(claim for claim in owner_y["claims"] if claim["id"] == owner_y_id)
    require(owner_y_claim["addPlayer"]["name"] == "Player Y",
            f"claim stored spoofed player metadata: {owner_y_claim!r}")
    require(owner_y_claim["processingPeriod"] == PROCESSING_PERIOD,
            f"claim did not persist processing period: {owner_y_claim!r}")
    version = int(owner_y["version"])

    owner_x = expect(
        transaction(
            league_id,
            tokens[emails[0]],
            f"owner-x-{RUN_KEY}",
            "create",
            version,
            addPlayerId=f"player-x-{RUN_KEY}",
        ),
        200,
        "owner claim X",
    )
    owner_x_id = str(owner_x["claimId"])
    version = int(owner_x["version"])

    reordered = expect(
        transaction(
            league_id,
            tokens[emails[0]],
            f"owner-reorder-{RUN_KEY}",
            "reorder",
            version,
            claimIds=[owner_x_id, owner_y_id],
        ),
        200,
        "owner exact reorder",
    )
    version = int(reordered["version"])
    owner_pending = [
        claim for claim in reordered["claims"]
        if claim["managerEmail"] == emails[0] and claim["status"] == "Pending"
    ]
    require([claim["id"] for claim in owner_pending] == [owner_x_id, owner_y_id],
            f"owner reorder was not persisted: {owner_pending!r}")

    member_x = expect(
        transaction(
            league_id,
            tokens[emails[1]],
            f"member-x-{RUN_KEY}",
            "create",
            version,
            addPlayerId=f"player-x-{RUN_KEY}",
        ),
        200,
        "member competing X claim",
    )
    member_x_id = str(member_x["claimId"])
    version = int(member_x["version"])

    member_z = expect(
        transaction(
            league_id,
            tokens[emails[1]],
            f"member-z-{RUN_KEY}",
            "create",
            version,
            addPlayerId=f"player-z-{RUN_KEY}",
        ),
        200,
        "member claim Z",
    )
    member_z_id = str(member_z["claimId"])
    version = int(member_z["version"])

    cancel_claim = expect(
        transaction(
            league_id,
            tokens[emails[2]],
            f"member2-create-{RUN_KEY}",
            "create",
            version,
            addPlayerId=f"player-cancel-{RUN_KEY}",
        ),
        200,
        "member2 cancellable claim",
    )
    cancel_id = str(cancel_claim["claimId"])
    version = int(cancel_claim["version"])
    cancel_key = f"member2-cancel-{RUN_KEY}"
    cancelled = expect(
        transaction(
            league_id,
            tokens[emails[2]],
            cancel_key,
            "cancel",
            version,
            claimId=cancel_id,
        ),
        200,
        "cancel pending claim",
    )
    version = int(cancelled["version"])
    cancel_replay = expect(
        transaction(
            league_id,
            tokens[emails[2]],
            cancel_key,
            "cancel",
            version - 1,
            claimId=cancel_id,
        ),
        200,
        "cancel replay",
    )
    require(cancel_replay.get("idempotentReplay") is True,
            f"cancel replay marker absent: {cancel_replay!r}")
    require(int(cancel_replay["version"]) == version,
            f"cancel replay changed version: {cancel_replay!r}")

    stale = expect(
        transaction(
            league_id,
            tokens[emails[3]],
            f"stale-create-{RUN_KEY}",
            "create",
            version - 1,
            addPlayerId=f"player-z-{RUN_KEY}",
        ),
        409,
        "stale claim creation",
    )
    require(stale.get("code") == "waiver_state_conflict", f"wrong stale code: {stale!r}")
    require(int(stale.get("state", {}).get("version", -1)) == version,
            f"stale conflict omitted authoritative state: {stale!r}")

    close_processing_period(league_id)
    closed_state = expect(
        call("GET", f"/api/leagues/{league_id}/waivers/state", token=tokens[emails[0]]),
        200,
        "closed waiver state",
    )
    require(closed_state.get("deadlinePassed") is True and closed_state.get("claimsMutable") is False,
            f"closed period still allows claim management: {closed_state!r}")
    require(closed_state.get("processingPeriod") == PROCESSING_PERIOD,
            f"deadline transition changed processing period identity: {closed_state!r}")

    late_reorder = expect(
        transaction(
            league_id,
            tokens[emails[0]],
            f"late-reorder-{RUN_KEY}",
            "reorder",
            version,
            claimIds=[owner_x_id, owner_y_id],
        ),
        409,
        "post-deadline reorder",
    )
    require(late_reorder.get("code") == "waiver_deadline_passed",
            f"post-deadline reorder was not blocked: {late_reorder!r}")

    out_of_order = expect(
        transaction(
            league_id,
            tokens[emails[0]],
            f"out-of-order-{RUN_KEY}",
            "process_one",
            version,
            claimId=owner_y_id,
        ),
        409,
        "out-of-order individual process",
    )
    require(out_of_order.get("code") == "waiver_claim_out_of_order",
            f"single claim bypassed ordering: {out_of_order!r}")
    require(out_of_order.get("nextClaimId") == owner_x_id,
            f"next claim identity wrong: {out_of_order!r}")

    process_key = f"process-all-{RUN_KEY}"
    processed = expect(
        transaction(
            league_id,
            tokens[emails[0]],
            process_key,
            "process",
            version,
        ),
        200,
        "deterministic waiver run",
    )
    final_version = int(processed["version"])
    require(processed.get("processed") == [owner_x_id, member_z_id, owner_y_id],
            f"dynamic priority processing order wrong: {processed.get('processed')!r}")
    require(processed.get("failed") == [member_x_id],
            f"competing losing claim was not the sole failure: {processed.get('failed')!r}")
    require(processed.get("pendingCount") == 0, f"waiver run left pending claims: {processed!r}")
    require(processed.get("lastProcessingPeriod") == PROCESSING_PERIOD,
            f"completed processing period was not persisted: {processed!r}")

    require(db_player_owner(league_id, f"player-x-{RUN_KEY}") == emails[0],
            "owner did not win top-priority player X")
    require(db_player_owner(league_id, f"player-y-{RUN_KEY}") == emails[0],
            "owner did not receive second claim after dynamic rotation")
    require(db_player_owner(league_id, f"player-z-{RUN_KEY}") == emails[1],
            "member did not win player Z")
    require(db_roster_snapshot_name(league_id, f"player-y-{RUN_KEY}") == "Player Y",
            "processed roster stored spoofed claim metadata instead of canonical player state")

    successful_claim = next(claim for claim in processed["claims"] if claim["id"] == owner_x_id)
    require(successful_claim["status"] == "Successful",
            f"successful claim did not expose successful status: {successful_claim!r}")
    failed_claim = next(claim for claim in processed["claims"] if claim["id"] == member_x_id)
    require(failed_claim["status"] == "Failed" and failed_claim["failureCode"] == "player_unavailable",
            f"losing same-player claim failure state wrong: {failed_claim!r}")
    require(bool(failed_claim.get("failureReason")),
            f"losing claim omitted readable failure reason: {failed_claim!r}")

    priority_emails = [entry["managerEmail"] for entry in processed["priority"]]
    require(priority_emails == [emails[2], emails[3], emails[1], emails[0]],
            f"dynamic priority rotation wrong: {priority_emails!r}")

    roster_versions: dict[str, int] = {}
    with psycopg.connect(DB_URL) as connection:
        with connection.cursor() as cursor:
            cursor.execute(
                "SELECT lower(manager_email), version FROM roster_states "
                "WHERE league_id = %s AND lower(manager_email) IN (%s, %s)",
                (league_id, emails[0], emails[1]),
            )
            roster_versions = {str(email): int(version_value) for email, version_value in cursor.fetchall()}
            cursor.execute(
                "SELECT COUNT(*) FROM rosters WHERE league_id = %s AND player_id IN (%s, %s, %s)",
                (league_id, f"player-x-{RUN_KEY}", f"player-y-{RUN_KEY}", f"player-z-{RUN_KEY}"),
            )
            require(int(cursor.fetchone()[0]) == 3, "processed roster ownership count is wrong")
            cursor.execute(
                "SELECT COUNT(*) FROM waiver_operations WHERE league_id = %s AND operation_key = %s",
                (league_id, process_key),
            )
            require(int(cursor.fetchone()[0]) == 1, "processing operation was not recorded exactly once")
    require(roster_versions.get(emails[0]) == 2,
            f"owner roster version should advance twice: {roster_versions!r}")
    require(roster_versions.get(emails[1]) == 1,
            f"member roster version should advance once: {roster_versions!r}")

    replay = expect(
        transaction(
            league_id,
            tokens[emails[0]],
            process_key,
            "process",
            version,
        ),
        200,
        "waiver run replay",
    )
    require(replay.get("idempotentReplay") is True, f"processing replay marker absent: {replay!r}")
    require(int(replay["version"]) == final_version,
            f"processing replay advanced version: {replay!r}")
    require(replay.get("processed") == processed.get("processed"),
            f"processing replay changed results: {replay!r}")

    period_retry = expect(
        transaction(
            league_id,
            tokens[emails[0]],
            f"process-period-retry-{RUN_KEY}",
            "process",
            version,
        ),
        200,
        "same-period semantic retry",
    )
    require(period_retry.get("periodAlreadyProcessed") is True,
            f"same processing period was not recognized as complete: {period_retry!r}")
    require(int(period_retry["version"]) == final_version,
            f"same-period retry changed waiver version: {period_retry!r}")
    require(db_player_owner(league_id, f"player-x-{RUN_KEY}") == emails[0],
            "same-period retry changed player ownership")

    print(json.dumps({
        "status": "ok",
        "leagueId": league_id,
        "processingPeriod": PROCESSING_PERIOD,
        "processed": processed["processed"],
        "failed": processed["failed"],
        "finalVersion": final_version,
        "priority": priority_emails,
        "rosterVersions": roster_versions,
    }, sort_keys=True))


if __name__ == "__main__":
    try:
        main()
    except ContractFailure as error:
        print(f"waiver lifecycle contract failure: {error}", flush=True)
        raise SystemExit(1) from error
