#!/usr/bin/env python3
"""Database-backed contracts for roster transaction concurrency and recovery."""
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
PASSWORD = os.getenv("CFF_CONTRACT_PASSWORD", "Roster-Contract-Password-2026!")
RUN_KEY = os.getenv("CFF_ROSTER_RUN_KEY", str(time.time_ns()))


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


def configure_member(league_id: str, member_email: str) -> None:
    with psycopg.connect(DB_URL) as connection:
        with connection.cursor() as cursor:
            cursor.execute(
                "UPDATE league_members SET status = 'active', joined_at = NOW(), updated_at = NOW() "
                "WHERE league_id = %s AND lower(email) = lower(%s)",
                (league_id, member_email),
            )
            require(cursor.rowcount == 1, "invited roster contract member was not activated")
        connection.commit()


def set_waiver_mode(league_id: str, mode: str, locked: bool = False) -> None:
    rules = json.dumps({"mode": mode, "claimDeadline": "", "freeAgencyLocked": locked})
    with psycopg.connect(DB_URL) as connection:
        with connection.cursor() as cursor:
            cursor.execute(
                "UPDATE leagues SET waiver_rules = %s::jsonb, updated_at = NOW() WHERE id = %s",
                (rules, league_id),
            )
            require(cursor.rowcount == 1, "league waiver mode was not updated")
        connection.commit()


def main() -> None:
    wait_for_api()
    owner = f"roster-owner-{RUN_KEY}@example.test"
    member = f"roster-member-{RUN_KEY}@example.test"
    owner_token = signup(owner)
    member_token = signup(member)

    roster_rules = {"qb": 1, "rb": 0, "wr": 0, "te": 0, "flex": 0, "bench": 0}
    created = expect(
        call(
            "POST",
            "/api/leagues",
            token=owner_token,
            operation_key=f"create-roster-{RUN_KEY}",
            payload={
                "name": f"Roster Contract {RUN_KEY}",
                "teams": 4,
                "scoring": "ppr",
                "draftType": "snake",
                "invitedEmails": [member],
                "rosterRules": roster_rules,
                "waiverRules": {"mode": "free_agency", "claimDeadline": "", "freeAgencyLocked": False},
            },
        ),
        201,
        "create roster contract league",
    )
    league_id = str(created.get("id", ""))
    require(league_id, f"league ID missing: {created!r}")
    configure_member(league_id, member)

    owner_state = expect(
        call("GET", f"/api/leagues/{league_id}/roster/state", token=owner_token),
        200,
        "owner roster state",
    )
    member_state = expect(
        call("GET", f"/api/leagues/{league_id}/roster/state", token=member_token),
        200,
        "member roster state",
    )
    require(owner_state.get("version") == 0 and member_state.get("version") == 0,
            f"initial roster versions were not zero: {owner_state!r} {member_state!r}")

    shared_player = {
        "id": f"shared-qb-{RUN_KEY}",
        "name": "Shared Quarterback",
        "position": "QB",
        "team": "Contract U",
    }

    def race(token: str, label: str) -> Response:
        return call(
            "POST",
            f"/api/leagues/{league_id}/roster/transactions",
            token=token,
            operation_key=f"race-{label}-{RUN_KEY}",
            payload={"action": "add", "expectedVersion": 0, "addPlayer": shared_player},
        )

    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
        race_responses = list(executor.map(
            lambda args: race(*args),
            ((owner_token, "owner"), (member_token, "member")),
        ))

    statuses = sorted(response.status for response in race_responses)
    require(statuses == [200, 409], f"same-player race was not one winner/one conflict: {statuses}")
    winner_index = next(index for index, response in enumerate(race_responses) if response.status == 200)
    winner_token = owner_token if winner_index == 0 else member_token
    loser_token = member_token if winner_index == 0 else owner_token
    winner_label = "owner" if winner_index == 0 else "member"
    winner = race_responses[winner_index].json()
    loser = next(response.json() for response in race_responses if response.status == 409)
    require(loser.get("code") == "player_unavailable", f"race conflict code wrong: {loser!r}")
    require(len(winner.get("roster", [])) == 1 and winner.get("version") == 1,
            f"winner roster state wrong: {winner!r}")

    replay = expect(
        call(
            "POST",
            f"/api/leagues/{league_id}/roster/transactions",
            token=winner_token,
            operation_key=f"race-{winner_label}-{RUN_KEY}",
            payload={"action": "add", "expectedVersion": 0, "addPlayer": shared_player},
        ),
        200,
        "accepted add replay",
    )
    require(replay.get("idempotentReplay") is True, f"replay marker absent: {replay!r}")
    require(len(replay.get("roster", [])) == 1, f"replay duplicated the player: {replay!r}")

    loser_state = expect(
        call("GET", f"/api/leagues/{league_id}/roster/state", token=loser_token),
        200,
        "loser roster recovery",
    )
    loser_player = {
        "id": f"loser-qb-{RUN_KEY}",
        "name": "Loser Quarterback",
        "position": "QB",
        "team": "Recovery U",
    }
    loser_added = expect(
        call(
            "POST",
            f"/api/leagues/{league_id}/roster/transactions",
            token=loser_token,
            operation_key=f"loser-add-{RUN_KEY}",
            payload={
                "action": "add",
                "expectedVersion": int(loser_state["version"]),
                "addPlayer": loser_player,
            },
        ),
        200,
        "loser adds different player",
    )
    require(len(loser_added.get("roster", [])) == 1, f"loser add failed: {loser_added!r}")

    failed_swap = expect(
        call(
            "POST",
            f"/api/leagues/{league_id}/roster/transactions",
            token=winner_token,
            operation_key=f"failed-swap-{RUN_KEY}",
            payload={
                "action": "swap",
                "expectedVersion": int(winner["version"]),
                "addPlayer": loser_player,
                "dropPlayerId": shared_player["id"],
            },
        ),
        409,
        "failed swap to unavailable player",
    )
    require(failed_swap.get("code") == "player_unavailable", f"failed swap code wrong: {failed_swap!r}")
    after_failed_swap = expect(
        call("GET", f"/api/leagues/{league_id}/roster/state", token=winner_token),
        200,
        "failed swap rollback state",
    )
    require([item.get("id") for item in after_failed_swap.get("roster", [])] == [shared_player["id"]],
            f"failed swap partially dropped the original player: {after_failed_swap!r}")
    require(after_failed_swap.get("version") == winner.get("version"),
            f"failed swap advanced the roster version: {after_failed_swap!r}")

    replacement = {
        "id": f"replacement-qb-{RUN_KEY}",
        "name": "Replacement Quarterback",
        "position": "QB",
        "team": "Atomic U",
    }
    swapped = expect(
        call(
            "POST",
            f"/api/leagues/{league_id}/roster/transactions",
            token=winner_token,
            operation_key=f"successful-swap-{RUN_KEY}",
            payload={
                "action": "swap",
                "expectedVersion": int(after_failed_swap["version"]),
                "addPlayer": replacement,
                "dropPlayerId": shared_player["id"],
            },
        ),
        200,
        "successful atomic swap",
    )
    require([item.get("id") for item in swapped.get("roster", [])] == [replacement["id"]],
            f"successful swap did not replace exactly one player: {swapped!r}")
    require(swapped.get("version") == int(after_failed_swap["version"]) + 1,
            f"successful swap did not advance one revision: {swapped!r}")

    stale = expect(
        call(
            "POST",
            f"/api/leagues/{league_id}/roster/transactions",
            token=winner_token,
            operation_key=f"stale-drop-{RUN_KEY}",
            payload={
                "action": "drop",
                "expectedVersion": int(after_failed_swap["version"]),
                "playerId": replacement["id"],
            },
        ),
        409,
        "stale roster mutation",
    )
    require(stale.get("code") == "roster_state_conflict", f"stale conflict code wrong: {stale!r}")
    require(stale.get("state", {}).get("version") == swapped.get("version"),
            f"stale conflict did not include current state: {stale!r}")

    set_waiver_mode(league_id, "waivers")
    waiver_blocked = expect(
        call(
            "POST",
            f"/api/leagues/{league_id}/roster/transactions",
            token=winner_token,
            operation_key=f"waiver-block-{RUN_KEY}",
            payload={
                "action": "swap",
                "expectedVersion": int(swapped["version"]),
                "addPlayer": {"id": f"waiver-qb-{RUN_KEY}", "name": "Waiver QB", "position": "QB"},
                "dropPlayerId": replacement["id"],
            },
        ),
        409,
        "waiver-mode direct add",
    )
    require(waiver_blocked.get("code") == "waiver_claim_required",
            f"waiver gate code wrong: {waiver_blocked!r}")

    with psycopg.connect(DB_URL) as connection:
        with connection.cursor() as cursor:
            cursor.execute(
                "SELECT COUNT(*) FROM rosters WHERE league_id = %s AND player_id = %s",
                (league_id, shared_player["id"]),
            )
            shared_count = int(cursor.fetchone()[0])
            cursor.execute(
                "SELECT COUNT(*) FROM rosters WHERE league_id = %s AND player_id = %s",
                (league_id, replacement["id"]),
            )
            replacement_count = int(cursor.fetchone()[0])
            cursor.execute(
                "SELECT COUNT(*) FROM roster_operations WHERE league_id = %s AND operation_key LIKE %s",
                (league_id, f"race-%-{RUN_KEY}"),
            )
            race_operations = int(cursor.fetchone()[0])
    require(shared_count == 0, f"swapped-out player still has an owner: {shared_count}")
    require(replacement_count == 1, f"replacement player ownership is not unique: {replacement_count}")
    require(race_operations == 1, f"accepted race operation was not recorded exactly once: {race_operations}")

    print(json.dumps({
        "status": "ok",
        "leagueId": league_id,
        "raceStatuses": statuses,
        "winner": winner_label,
        "finalVersion": swapped.get("version"),
        "waiverGate": waiver_blocked.get("code"),
    }, sort_keys=True))


if __name__ == "__main__":
    try:
        main()
    except ContractFailure as error:
        print(f"roster transaction contract failure: {error}", flush=True)
        raise SystemExit(1) from error
