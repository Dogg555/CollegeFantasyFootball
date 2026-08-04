#!/usr/bin/env python3
"""Database-backed contracts for multiplayer draft concurrency and recovery."""
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


def main() -> None:
    wait_for_api()
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
                "name": f"Draft Contract {RUN_KEY}",
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

    snapshots: dict[str, dict[str, Any]] = {}
    for email in emails:
        snapshot = expect(
            call(
                "POST",
                f"/api/leagues/{league_id}/draft/readiness",
                token=tokens[email],
                payload={"ready": True},
            ),
            200,
            f"ready {email}",
        )
        snapshots[email] = snapshot

    lobby = snapshots[emails[-1]]
    require(lobby.get("allReady") is True, f"all-ready state absent: {lobby!r}")
    require(lobby.get("readyCount") == 4, f"ready count wrong: {lobby!r}")
    require(lobby.get("activeManagerCount") == 4, f"active manager count wrong: {lobby!r}")
    require(lobby.get("totalPicks") == 56, f"four-team completion target wrong: {lobby!r}")

    version0 = int(lobby.get("version", -1))
    started = expect(
        call(
            "POST",
            f"/api/leagues/{league_id}/draft/start",
            token=tokens[emails[0]],
            operation_key=f"start-{RUN_KEY}",
            payload={"expectedVersion": version0, "force": False},
        ),
        200,
        "start ready draft",
    )
    require(started.get("status") == "open", f"draft did not open: {started!r}")
    require(int(started.get("version", -1)) == version0 + 1, f"start revision did not advance: {started!r}")
    require(started.get("currentPick") == 1, f"draft did not start at pick one: {started!r}")

    current_manager = str(started.get("currentManager", "")).lower()
    require(current_manager in tokens, f"current manager is not an active account: {started!r}")
    pick_version = int(started["version"])
    pick_payloads = [
        {
            "expectedVersion": pick_version,
            "expectedPick": 1,
            "player": {"id": f"race-player-a-{RUN_KEY}", "name": "Race Player A", "position": "QB", "team": "A"},
        },
        {
            "expectedVersion": pick_version,
            "expectedPick": 1,
            "player": {"id": f"race-player-b-{RUN_KEY}", "name": "Race Player B", "position": "QB", "team": "B"},
        },
    ]

    def submit(index: int) -> Response:
        return call(
            "POST",
            f"/api/leagues/{league_id}/draft/picks",
            token=tokens[current_manager],
            operation_key=f"pick-race-{RUN_KEY}-{index}",
            payload=pick_payloads[index],
        )

    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
        race_responses = list(executor.map(submit, (0, 1)))

    statuses = sorted(response.status for response in race_responses)
    require(statuses == [201, 409], f"simultaneous pick race was not one winner/one conflict: {statuses}")
    winner_index = next(index for index, response in enumerate(race_responses) if response.status == 201)
    loser = next(response.json() for response in race_responses if response.status == 409)
    require(loser.get("code") == "draft_state_conflict", f"race conflict code wrong: {loser!r}")

    winner = race_responses[winner_index].json()
    require(len(winner.get("picks", [])) == 1, f"winner did not return one confirmed pick: {winner!r}")
    require(winner.get("currentPick") == 2, f"winner did not advance the draft: {winner!r}")
    require(int(winner.get("version", -1)) == pick_version + 1, f"winner did not advance revision: {winner!r}")

    replay = expect(
        call(
            "POST",
            f"/api/leagues/{league_id}/draft/picks",
            token=tokens[current_manager],
            operation_key=f"pick-race-{RUN_KEY}-{winner_index}",
            payload=pick_payloads[winner_index],
        ),
        200,
        "replay accepted pick",
    )
    require(replay.get("idempotentReplay") is True, f"pick replay marker absent: {replay!r}")
    require(len(replay.get("picks", [])) == 1, f"pick replay created a duplicate: {replay!r}")

    latest = expect(
        call("GET", f"/api/leagues/{league_id}/draft", token=tokens[emails[0]]),
        200,
        "authoritative draft recovery",
    )
    require(len(latest.get("picks", [])) == 1, f"authoritative state has duplicate picks: {latest!r}")
    require(latest.get("currentPick") == 2, f"authoritative state has wrong current pick: {latest!r}")

    next_manager = str(latest.get("currentManager", "")).lower()
    stale = expect(
        call(
            "POST",
            f"/api/leagues/{league_id}/draft/picks",
            token=tokens[next_manager],
            operation_key=f"stale-pick-{RUN_KEY}",
            payload={
                "expectedVersion": pick_version,
                "expectedPick": 2,
                "player": {"id": f"stale-player-{RUN_KEY}", "name": "Stale Player", "position": "RB", "team": "S"},
            },
        ),
        409,
        "stale reconnect pick",
    )
    require(stale.get("code") == "draft_state_conflict", f"stale conflict code wrong: {stale!r}")

    latest_version = int(latest["version"])
    undone = expect(
        call(
            "POST",
            f"/api/leagues/{league_id}/draft/undo",
            token=tokens[emails[0]],
            operation_key=f"undo-{RUN_KEY}",
            payload={"expectedVersion": latest_version},
        ),
        200,
        "commissioner undo",
    )
    require(len(undone.get("picks", [])) == 0 and undone.get("currentPick") == 1, f"undo did not restore pick one: {undone!r}")

    undo_replay = expect(
        call(
            "POST",
            f"/api/leagues/{league_id}/draft/undo",
            token=tokens[emails[0]],
            operation_key=f"undo-{RUN_KEY}",
            payload={"expectedVersion": latest_version},
        ),
        200,
        "commissioner undo replay",
    )
    require(undo_replay.get("idempotentReplay") is True, f"undo replay marker absent: {undo_replay!r}")
    require(len(undo_replay.get("picks", [])) == 0, f"undo replay changed state: {undo_replay!r}")

    reset_version = int(undone["version"])
    reset = expect(
        call(
            "POST",
            f"/api/leagues/{league_id}/draft/reset",
            token=tokens[emails[0]],
            operation_key=f"reset-{RUN_KEY}",
            payload={"expectedVersion": reset_version},
        ),
        200,
        "commissioner reset",
    )
    require(reset.get("status") == "not_started", f"reset did not return to lobby: {reset!r}")
    require(reset.get("currentPick") == 1 and len(reset.get("picks", [])) == 0, f"reset state is inconsistent: {reset!r}")
    require(reset.get("readyCount") == 0 and reset.get("allReady") is False, f"reset did not clear readiness: {reset!r}")

    print(json.dumps({
        "status": "ok",
        "leagueId": league_id,
        "raceStatuses": statuses,
        "winningPlayer": winner.get("acceptedPlayerId"),
        "finalVersion": reset.get("version"),
        "totalPicks": lobby.get("totalPicks"),
    }, sort_keys=True))


if __name__ == "__main__":
    try:
        main()
    except ContractFailure as error:
        print(f"draft lifecycle contract failure: {error}", flush=True)
        raise SystemExit(1) from error
