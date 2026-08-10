#!/usr/bin/env python3
"""Production PostgreSQL contracts for the Phase 7 synchronized draft room."""
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
PASSWORD = os.getenv("CFF_CONTRACT_PASSWORD", "Draft-Contract-Password-2026!")
RUN_KEY = os.getenv("CFF_DRAFT_RUN_KEY", str(time.time_ns()))


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
        except Exception as exc:  # noqa: BLE001
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
    require(token.startswith("token-"), f"signup did not return a bearer token for {email}: {body!r}")
    return token


def configure_league(league_id: str, expected_members: int) -> None:
    with psycopg.connect(DB_URL) as connection:
        with connection.cursor() as cursor:
            cursor.execute(
                "UPDATE league_members SET status = 'active', joined_at = NOW(), updated_at = NOW() "
                "WHERE league_id = %s",
                (league_id,),
            )
            cursor.execute(
                "UPDATE leagues SET draft_lobby_open = TRUE, draft_lobby_started_at = NOW(), updated_at = NOW() "
                "WHERE id = %s",
                (league_id,),
            )
            cursor.execute(
                "SELECT COUNT(*) FROM league_members WHERE league_id = %s AND status = 'active'",
                (league_id,),
            )
            active = int(cursor.fetchone()[0])
            require(active == expected_members, f"expected {expected_members} active managers, found {active}")
        connection.commit()


def seed_players() -> dict[str, list[str]]:
    by_position: dict[str, list[str]] = {position: [] for position in ("QB", "RB", "WR", "TE")}
    rows: list[tuple[Any, ...]] = []
    for position in by_position:
        for index in range(80):
            player_id = f"phase7-{RUN_KEY}-{position.lower()}-{index:03d}"
            by_position[position].append(player_id)
            rows.append(
                (
                    player_id,
                    f"Phase Seven {position} {index:03d}",
                    position,
                    f"School {index % 16:02d}",
                    "Big Test",
                    "JR",
                    2026,
                    True,
                )
            )
    inactive_id = f"phase7-{RUN_KEY}-inactive"
    rows.append((inactive_id, "Inactive Draft Player", "QB", "Inactive U", "Big Test", "SR", 2026, False))
    with psycopg.connect(DB_URL) as connection:
        with connection.cursor() as cursor:
            cursor.executemany(
                "INSERT INTO players "
                "(id, full_name, position, team, conference, year, season, active, last_seen_at, updated_at) "
                "VALUES (%s, %s, %s, %s, %s, %s, %s, %s, NOW(), NOW()) "
                "ON CONFLICT (id) DO UPDATE SET full_name = EXCLUDED.full_name, position = EXCLUDED.position, "
                "team = EXCLUDED.team, conference = EXCLUDED.conference, year = EXCLUDED.year, "
                "season = EXCLUDED.season, active = EXCLUDED.active, updated_at = NOW()",
                rows,
            )
        connection.commit()
    by_position["inactive"] = [inactive_id]
    return by_position


def force_deadline_expired(league_id: str) -> None:
    with psycopg.connect(DB_URL) as connection:
        with connection.cursor() as cursor:
            cursor.execute(
                "UPDATE draft_states SET pick_deadline = NOW() - INTERVAL '1 second' WHERE league_id = %s",
                (league_id,),
            )
        connection.commit()


def queue_player(league_id: str, manager: str, player_id: str) -> None:
    spoofed = [{"id": player_id, "name": "Spoofed Queue Name", "position": "K", "team": "Spoof U"}]
    with psycopg.connect(DB_URL) as connection:
        with connection.cursor() as cursor:
            cursor.execute(
                "INSERT INTO draft_queues (league_id, manager_email, queue, updated_at) "
                "VALUES (%s, %s, %s::jsonb, NOW()) "
                "ON CONFLICT (league_id, manager_email) DO UPDATE SET queue = EXCLUDED.queue, updated_at = NOW()",
                (league_id, manager, json.dumps(spoofed)),
            )
        connection.commit()


def enable_auto_draft_for_all(league_id: str) -> None:
    with psycopg.connect(DB_URL) as connection:
        with connection.cursor() as cursor:
            cursor.execute(
                "UPDATE draft_readiness SET auto_draft_enabled = TRUE, last_seen_at = NOW(), updated_at = NOW() "
                "WHERE league_id = %s",
                (league_id,),
            )
        connection.commit()


def roster_counts(league_id: str) -> dict[str, int]:
    with psycopg.connect(DB_URL) as connection:
        with connection.cursor() as cursor:
            cursor.execute(
                "SELECT lower(manager_email), COUNT(*) FROM rosters "
                "WHERE league_id = %s AND acquired_via = 'draft' GROUP BY lower(manager_email)",
                (league_id,),
            )
            return {str(email): int(count) for email, count in cursor.fetchall()}


def drafted_player_count(league_id: str, player_id: str) -> int:
    with psycopg.connect(DB_URL) as connection:
        with connection.cursor() as cursor:
            cursor.execute(
                "SELECT COUNT(*) FROM draft_picks WHERE league_id = %s AND player_id = %s",
                (league_id, player_id),
            )
            return int(cursor.fetchone()[0])


def main() -> None:
    wait_for_api()
    players = seed_players()
    emails = [
        f"draft-owner-{RUN_KEY}@example.test",
        f"draft-member1-{RUN_KEY}@example.test",
        f"draft-member2-{RUN_KEY}@example.test",
        f"draft-member3-{RUN_KEY}@example.test",
    ]
    tokens = {email: signup(email) for email in emails}

    create = expect(
        call(
            "POST",
            "/api/leagues",
            token=tokens[emails[0]],
            operation_key=f"create-{RUN_KEY}",
            payload={
                "name": f"Draft Phase 7 {RUN_KEY}",
                "teams": 4,
                "scoring": "ppr",
                "draftType": "snake",
                "invitedEmails": emails[1:],
                "rosterRules": {"qb": 1, "rb": 2, "wr": 2, "te": 1, "flex": 2, "bench": 6},
            },
        ),
        201,
        "create four-team league",
    )
    league_id = str(create.get("id", ""))
    require(league_id, f"league ID missing: {create!r}")
    configure_league(league_id, 4)

    lobby = expect(call("GET", f"/api/leagues/{league_id}/draft", token=tokens[emails[0]]), 200, "draft lobby snapshot")
    require(lobby.get("lifecycleState") == "LOBBY_OPEN", f"lobby lifecycle state wrong: {lobby!r}")
    require(lobby.get("totalPicks") == 56, f"four-team completion target wrong: {lobby!r}")

    settings = expect(
        call(
            "POST",
            f"/api/leagues/{league_id}/draft/settings",
            token=tokens[emails[0]],
            operation_key=f"settings-{RUN_KEY}",
            payload={"expectedVersion": int(lobby["version"]), "pickClockSeconds": 30, "timezone": "America/Denver"},
        ),
        200,
        "save server draft settings",
    )
    require(settings.get("pickClockSeconds") == 30, f"pick duration did not persist: {settings!r}")
    require(settings.get("draftTimezone") == "America/Denver", f"draft timezone did not persist: {settings!r}")

    cancelled = expect(
        call(
            "POST",
            f"/api/leagues/{league_id}/draft/cancel",
            token=tokens[emails[0]],
            operation_key=f"cancel-{RUN_KEY}",
            payload={"expectedVersion": int(settings["version"])},
        ),
        200,
        "cancel pre-start draft",
    )
    require(cancelled.get("lifecycleState") == "CANCELLED", f"cancel lifecycle state wrong: {cancelled!r}")
    blocked_start = expect(
        call(
            "POST",
            f"/api/leagues/{league_id}/draft/start",
            token=tokens[emails[0]],
            operation_key=f"cancelled-start-{RUN_KEY}",
            payload={"expectedVersion": int(cancelled["version"]), "force": True},
        ),
        409,
        "cancelled draft cannot start",
    )
    require(blocked_start.get("code") == "draft_reset_required", f"cancelled start code wrong: {blocked_start!r}")

    reset = expect(
        call(
            "POST",
            f"/api/leagues/{league_id}/draft/reset",
            token=tokens[emails[0]],
            operation_key=f"reset-{RUN_KEY}",
            payload={"expectedVersion": int(cancelled["version"])},
        ),
        200,
        "reset cancelled draft",
    )
    require(reset.get("status") == "not_started" and reset.get("lifecycleState") == "LOBBY_OPEN", f"reset lifecycle wrong: {reset!r}")
    require(reset.get("pickClockSeconds") == 30 and reset.get("draftTimezone") == "America/Denver", "reset must preserve configured clock/timezone")

    snapshots: dict[str, dict[str, Any]] = {}
    for email in emails:
        snapshot = expect(
            call("POST", f"/api/leagues/{league_id}/draft/readiness", token=tokens[email], payload={"ready": True}),
            200,
            f"ready {email}",
        )
        snapshots[email] = snapshot
    lobby = snapshots[emails[-1]]
    require(lobby.get("allReady") is True and lobby.get("readyCount") == 4, f"all-ready state absent: {lobby!r}")

    started = expect(
        call(
            "POST",
            f"/api/leagues/{league_id}/draft/start",
            token=tokens[emails[0]],
            operation_key=f"start-{RUN_KEY}",
            payload={"expectedVersion": int(lobby["version"]), "force": False},
        ),
        200,
        "start ready draft",
    )
    require(started.get("status") == "open" and started.get("lifecycleState") == "IN_PROGRESS", f"draft did not enter IN_PROGRESS: {started!r}")
    require(started.get("currentPick") == 1, f"draft did not start at pick one: {started!r}")
    order = [str(value).lower() for value in started.get("draftOrder", [])]
    require(len(order) == 4, f"draft order missing managers: {started!r}")

    current_manager = str(started.get("currentManager", "")).lower()
    wrong_manager = next(email for email in emails if email != current_manager)
    wrong_turn = expect(
        call(
            "POST",
            f"/api/leagues/{league_id}/draft/picks",
            token=tokens[wrong_manager],
            operation_key=f"wrong-turn-{RUN_KEY}",
            payload={
                "expectedVersion": int(started["version"]),
                "expectedPick": 1,
                "player": {"id": players["QB"][0], "name": "Spoofed", "position": "TE", "team": "Spoof U"},
            },
        ),
        409,
        "wrong manager pick",
    )
    require(wrong_turn.get("code") == "draft_not_your_turn", f"wrong-turn code wrong: {wrong_turn!r}")

    inactive = expect(
        call(
            "POST",
            f"/api/leagues/{league_id}/draft/picks",
            token=tokens[current_manager],
            operation_key=f"inactive-{RUN_KEY}",
            payload={
                "expectedVersion": int(started["version"]),
                "expectedPick": 1,
                "player": {"id": players["inactive"][0], "name": "Pretend Active", "position": "QB"},
            },
        ),
        409,
        "inactive player rejection",
    )
    require(inactive.get("code") == "draft_player_ineligible", f"inactive player code wrong: {inactive!r}")

    race_player = players["QB"][1]
    pick_payload = {
        "expectedVersion": int(started["version"]),
        "expectedPick": 1,
        "player": {"id": race_player, "name": "Client Spoof Name", "position": "TE", "team": "Spoof U"},
    }

    def submit_race(index: int) -> Response:
        return call(
            "POST",
            f"/api/leagues/{league_id}/draft/picks",
            token=tokens[current_manager],
            operation_key=f"same-player-race-{RUN_KEY}-{index}",
            payload=pick_payload,
        )

    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
        race_responses = list(executor.map(submit_race, (0, 1)))
    race_statuses = sorted(response.status for response in race_responses)
    require(race_statuses == [201, 409], f"same-player race was not one winner/one conflict: {race_statuses}")
    winner = next(response.json() for response in race_responses if response.status == 201)
    loser = next(response.json() for response in race_responses if response.status == 409)
    require(loser.get("code") == "draft_state_conflict", f"race conflict code wrong: {loser!r}")
    require(drafted_player_count(league_id, race_player) == 1, "same player was drafted more than once")
    first_pick = winner.get("picks", [])[0]
    require(first_pick.get("player", {}).get("name") == "Phase Seven QB 001", f"server persisted spoofed player snapshot: {first_pick!r}")
    require(first_pick.get("player", {}).get("position") == "QB", f"server persisted spoofed player position: {first_pick!r}")

    paused = expect(
        call(
            "POST",
            f"/api/leagues/{league_id}/draft/pause",
            token=tokens[emails[0]],
            operation_key=f"pause-{RUN_KEY}",
            payload={"expectedVersion": int(winner["version"])},
        ),
        200,
        "commissioner pause",
    )
    require(paused.get("lifecycleState") == "PAUSED" and paused.get("status") == "paused", f"pause state wrong: {paused!r}")
    require(int(paused.get("pausedRemainingSeconds", 0)) > 0, f"pause did not preserve clock: {paused!r}")

    paused_manager = str(paused.get("currentManager", "")).lower()
    paused_pick = expect(
        call(
            "POST",
            f"/api/leagues/{league_id}/draft/picks",
            token=tokens[paused_manager],
            operation_key=f"paused-pick-{RUN_KEY}",
            payload={
                "expectedVersion": int(paused["version"]),
                "expectedPick": int(paused["currentPick"]),
                "playerId": players["RB"][0],
            },
        ),
        409,
        "manual pick while paused",
    )
    require(paused_pick.get("code") == "draft_paused", f"paused pick code wrong: {paused_pick!r}")

    resumed = expect(
        call(
            "POST",
            f"/api/leagues/{league_id}/draft/resume",
            token=tokens[emails[0]],
            operation_key=f"resume-{RUN_KEY}",
            payload={"expectedVersion": int(paused["version"])},
        ),
        200,
        "commissioner resume",
    )
    require(resumed.get("lifecycleState") == "IN_PROGRESS" and resumed.get("pickDeadline"), f"resume state wrong: {resumed!r}")

    timer_manager = str(resumed.get("currentManager", "")).lower()
    queue_player_id = players["RB"][1]
    queue_player(league_id, timer_manager, queue_player_id)
    force_deadline_expired(league_id)

    def refresh_for(email: str) -> Response:
        return call("GET", f"/api/leagues/{league_id}/draft", token=tokens[email])

    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
        timer_responses = list(executor.map(refresh_for, emails[:2]))
    require(all(response.status == 200 for response in timer_responses), f"timer refreshes failed: {[r.status for r in timer_responses]}")
    timer_snapshot = max((response.json() for response in timer_responses), key=lambda value: int(value.get("version", 0)))
    require(len(timer_snapshot.get("picks", [])) == 2, f"timer expiry should create exactly one additional pick: {timer_snapshot!r}")
    timer_pick = timer_snapshot["picks"][1]
    require(timer_pick.get("playerId") == queue_player_id, f"timer did not prefer queue: {timer_pick!r}")
    require(timer_pick.get("selectionSource") == "personal_queue" and timer_pick.get("automatic") is True, f"timer source wrong: {timer_pick!r}")
    require(timer_pick.get("player", {}).get("name") == "Phase Seven RB 001", f"queue auto-pick trusted spoofed snapshot: {timer_pick!r}")
    require(drafted_player_count(league_id, queue_player_id) == 1, "concurrent timer refresh produced duplicate auto-pick")

    reconnect = expect(call("GET", f"/api/leagues/{league_id}/draft", token=tokens[emails[2]]), 200, "reconnect authoritative snapshot")
    require(reconnect.get("currentPick") == timer_snapshot.get("currentPick"), f"reconnect current pick mismatch: {reconnect!r}")
    require(int(reconnect.get("version", -1)) == int(timer_snapshot.get("version", -2)), f"reconnect version mismatch: {reconnect!r}")
    require(len(reconnect.get("picks", [])) == 2, f"reconnect lost confirmed picks: {reconnect!r}")

    enable_auto_draft_for_all(league_id)
    completion = reconnect
    for _ in range(3):
        completion = expect(call("GET", f"/api/leagues/{league_id}/draft", token=tokens[emails[0]], timeout=30), 200, "auto-draft completion sync")
        if completion.get("lifecycleState") == "COMPLETED":
            break
    require(completion.get("status") == "complete" and completion.get("lifecycleState") == "COMPLETED", f"draft did not complete: {completion!r}")
    require(completion.get("draftFinalized") is True, f"completion did not mark finalized roster state: {completion!r}")
    require(len(completion.get("picks", [])) == 56 and completion.get("picksRemaining") == 0, f"completed draft pick count wrong: {completion!r}")
    require(completion.get("recap", {}).get("available") is True, f"completed draft recap missing: {completion!r}")
    require(completion.get("recap", {}).get("totalSelections") == 56, f"draft recap total wrong: {completion!r}")

    first_eight = [str(pick.get("managerEmail", "")).lower() for pick in completion["picks"][:8]]
    expected_snake = [order[0], order[1], order[2], order[3], order[3], order[2], order[1], order[0]]
    require(first_eight == expected_snake, f"snake reversal wrong: expected {expected_snake}, got {first_eight}")

    counts = roster_counts(league_id)
    require(set(counts) == set(emails), f"final rosters missing managers: {counts!r}")
    require(all(count == 14 for count in counts.values()), f"final rosters are not complete: {counts!r}")
    require(sum(counts.values()) == 56, f"finalized roster total wrong: {counts!r}")

    print(json.dumps({
        "status": "passed",
        "leagueId": league_id,
        "lifecycle": completion.get("lifecycleState"),
        "samePlayerRace": race_statuses,
        "timerExactlyOnce": True,
        "queueAutoPick": queue_player_id,
        "snakeFirstEight": first_eight,
        "finalRosterCounts": counts,
        "totalPicks": len(completion.get("picks", [])),
        "recap": completion.get("recap"),
    }, sort_keys=True))


if __name__ == "__main__":
    try:
        main()
    except ContractFailure as error:
        print(f"draft lifecycle contract failure: {error}", flush=True)
        raise SystemExit(1) from error
