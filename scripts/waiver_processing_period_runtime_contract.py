#!/usr/bin/env python3
"""Production regression for stable waiver processing periods without an explicit period key."""
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
PASSWORD = os.getenv("CFF_CONTRACT_PASSWORD", "Waiver-Period-Contract-2026!")
RUN_KEY = os.getenv("CFF_WAIVER_RUN_KEY", str(time.time_ns())) + "-period"
OPEN_DEADLINE = "2999-01-01T00:00"
EXTENDED_DEADLINE = "2999-02-01T00:00"
CLOSED_DEADLINE = "2000-01-01T00:00"


class ContractFailure(RuntimeError):
    pass


@dataclass(frozen=True)
class Response:
    status: int
    body: bytes

    def json(self) -> Any:
        return json.loads(self.body.decode()) if self.body else None


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ContractFailure(message)


def call(method: str, path: str, *, token: str = "", payload: Any | None = None, key: str = "") -> Response:
    headers = {"Accept": "application/json", "Origin": ORIGIN}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    if key:
        headers["Idempotency-Key"] = key
    data = None
    if payload is not None:
        data = json.dumps(payload).encode()
        headers["Content-Type"] = "application/json"
    request = urllib.request.Request(BASE + path, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(request, timeout=20) as response:
            return Response(response.status, response.read())
    except urllib.error.HTTPError as error:
        return Response(error.code, error.read())
    except urllib.error.URLError as error:
        raise ContractFailure(f"{method} {path} could not reach API: {error}") from error


def expect(response: Response, status: int, label: str) -> Any:
    body = response.json()
    require(response.status == status, f"{label}: expected {status}, got {response.status}: {body!r}")
    return body


def wait_for_api() -> None:
    for _ in range(60):
        try:
            if call("GET", "/api/auth/status").status == 200:
                return
        except ContractFailure:
            pass
        time.sleep(1)
    raise ContractFailure("API did not become ready")


def signup(email: str) -> str:
    body = expect(
        call("POST", "/api/auth/signup", payload={"email": email, "password": PASSWORD}),
        201,
        "signup",
    )
    token = str(body.get("token", ""))
    require(token, f"signup did not return token: {body!r}")
    return token


def set_deadline(league_id: str, deadline: str) -> None:
    with psycopg.connect(DB_URL) as connection:
        with connection.cursor() as cursor:
            cursor.execute(
                "UPDATE leagues SET waiver_rules = jsonb_set(waiver_rules, '{claimDeadline}', to_jsonb(%s::text)), "
                "updated_at = NOW() WHERE id = %s",
                (deadline, league_id),
            )
        connection.commit()


def main() -> None:
    wait_for_api()
    email = f"waiver-period-owner-{RUN_KEY}@example.test"
    token = signup(email)
    created = expect(
        call(
            "POST",
            "/api/leagues",
            token=token,
            key=f"create-league-{RUN_KEY}",
            payload={
                "name": f"Waiver Period Contract {RUN_KEY}",
                "teams": 4,
                "scoring": "ppr",
                "draftType": "snake",
                "rosterRules": {"qb": 1, "rb": 2, "wr": 2, "te": 1, "flex": 2, "bench": 6},
            },
        ),
        201,
        "create league",
    )
    league_id = str(created.get("id", ""))
    require(league_id, f"league ID missing: {created!r}")
    player_id = f"period-player-{RUN_KEY}"

    with psycopg.connect(DB_URL) as connection:
        with connection.cursor() as cursor:
            cursor.execute(
                "UPDATE leagues SET waiver_rules = %s::jsonb, updated_at = NOW() WHERE id = %s",
                (json.dumps({"mode": "waivers", "claimDeadline": OPEN_DEADLINE, "freeAgencyLocked": True}), league_id),
            )
            cursor.execute(
                "INSERT INTO players (id, full_name, position, team, conference, year, active, season, updated_at) "
                "VALUES (%s, 'Stable Period Player', 'WR', 'Period U', 'Contract Conference', 'JR', TRUE, 2026, NOW()) "
                "ON CONFLICT (id) DO UPDATE SET full_name = EXCLUDED.full_name, position = 'WR', team = 'Period U', "
                "conference = 'Contract Conference', year = 'JR', active = TRUE, season = 2026, updated_at = NOW()",
                (player_id,),
            )
            cursor.execute(
                "INSERT INTO waiver_states (league_id, version, current_processing_period, last_processing_period, updated_at) "
                "VALUES (%s, 0, '', '', NOW()) ON CONFLICT (league_id) DO UPDATE SET version = 0, "
                "current_processing_period = '', last_processing_period = '', last_processed_at = NULL, "
                "last_processing_run_id = NULL, updated_at = NOW()",
                (league_id,),
            )
        connection.commit()

    state = expect(call("GET", f"/api/leagues/{league_id}/waivers/state", token=token), 200, "initial state")
    period = str(state.get("processingPeriod", ""))
    require(period, f"server did not establish a persisted processing period: {state!r}")
    require(state.get("claimsMutable") is True, f"initial claim window is not open: {state!r}")

    claim = expect(
        call(
            "POST",
            f"/api/leagues/{league_id}/waivers/transactions",
            token=token,
            key=f"claim-{RUN_KEY}",
            payload={"action": "create", "expectedVersion": int(state["version"]), "addPlayerId": player_id},
        ),
        200,
        "create claim",
    )
    claim_id = str(claim.get("claimId", ""))
    require(claim_id, f"claim ID missing: {claim!r}")
    stored = next(item for item in claim["claims"] if item["id"] == claim_id)
    require(stored.get("processingPeriod") == period, f"claim used a different period: {stored!r}")

    set_deadline(league_id, EXTENDED_DEADLINE)
    extended = expect(call("GET", f"/api/leagues/{league_id}/waivers/state", token=token), 200, "extended deadline state")
    require(extended.get("processingPeriod") == period,
            f"editing an open deadline changed processing identity: before={period!r} after={extended.get('processingPeriod')!r}")
    extended_claim = next(item for item in extended["claims"] if item["id"] == claim_id)
    require(extended_claim.get("status") == "Pending", f"deadline edit expired a valid claim: {extended_claim!r}")

    set_deadline(league_id, CLOSED_DEADLINE)
    closed = expect(call("GET", f"/api/leagues/{league_id}/waivers/state", token=token), 200, "closed deadline state")
    require(closed.get("processingPeriod") == period, f"closing deadline changed processing identity: {closed!r}")
    require(closed.get("claimsMutable") is False and closed.get("canProcess") is True,
            f"closed period is not ready for processing: {closed!r}")

    processed = expect(
        call(
            "POST",
            f"/api/leagues/{league_id}/waivers/transactions",
            token=token,
            key=f"process-{RUN_KEY}",
            payload={"action": "process", "expectedVersion": int(closed["version"])},
        ),
        200,
        "process stable period",
    )
    require(processed.get("processed") == [claim_id], f"claim was not awarded: {processed!r}")
    require(processed.get("lastProcessingPeriod") == period and processed.get("periodProcessed") is True,
            f"completed period marker is unstable: {processed!r}")
    final_claim = next(item for item in processed["claims"] if item["id"] == claim_id)
    require(final_claim.get("status") == "Successful", f"claim did not complete successfully: {final_claim!r}")

    replay = expect(
        call(
            "POST",
            f"/api/leagues/{league_id}/waivers/transactions",
            token=token,
            key=f"process-second-key-{RUN_KEY}",
            payload={"action": "process", "expectedVersion": int(closed["version"])},
        ),
        200,
        "same-period second-key retry",
    )
    require(replay.get("periodAlreadyProcessed") is True,
            f"same completed period was processed again: {replay!r}")

    print(json.dumps({"status": "ok", "leagueId": league_id, "processingPeriod": period, "claimId": claim_id}, sort_keys=True))


if __name__ == "__main__":
    try:
        main()
    except ContractFailure as error:
        print(f"waiver processing period contract failure: {error}", flush=True)
        raise SystemExit(1) from error
