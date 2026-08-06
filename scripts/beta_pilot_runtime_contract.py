#!/usr/bin/env python3
"""Run a deterministic four-manager fantasy pilot against production API code.

The contract intentionally combines the hardened league, draft, roster, trade,
waiver, scoring, standings, authentication, and persistence boundaries in one
PostgreSQL-backed lifecycle. The database is used only to seed deterministic
college-player statistics needed for scoring assertions.
"""
from __future__ import annotations

import hashlib
import json
import os
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import psycopg

BASE = os.getenv("CFF_API_BASE_URL", "http://127.0.0.1:8080").rstrip("/")
DB_URL = os.environ["DB_URL"]
ORIGIN = os.getenv("CFF_CONTRACT_ORIGIN", "https://frontend.example.test")
PASSWORD = os.getenv("CFF_CONTRACT_PASSWORD", "Beta-Pilot-Contract-2026!")
RUN_KEY = os.getenv("CFF_BETA_PILOT_RUN_KEY", str(time.time_ns()))
REPORT_PATH = Path(os.getenv("CFF_BETA_PILOT_REPORT", "/tmp/beta-pilot-runtime.json"))
SEASON = int(os.getenv("CFF_BETA_PILOT_SEASON", "2026"))


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
            raise ContractFailure(
                f"HTTP {self.status} returned non-JSON: {self.body[:300]!r}"
            ) from exc


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ContractFailure(message)


def encoded(value: str) -> str:
    return urllib.parse.quote(value, safe="")


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
            return Response(
                response.status,
                {key.lower(): value for key, value in response.headers.items()},
                response.read(),
            )
    except urllib.error.HTTPError as error:
        return Response(
            error.code,
            {key.lower(): value for key, value in error.headers.items()},
            error.read(),
        )
    except urllib.error.URLError as error:
        raise ContractFailure(f"{method} {path} could not reach {BASE}: {error}") from error


def expect(response: Response, statuses: int | tuple[int, ...], label: str) -> Any:
    allowed = (statuses,) if isinstance(statuses, int) else statuses
    body = response.json()
    require(
        response.status in allowed,
        f"{label}: expected {allowed}, got {response.status}: {body!r}",
    )
    return body


def wait_for_api() -> None:
    last = "no response"
    for _ in range(90):
        try:
            response = call("GET", "/api/auth/status", timeout=3)
            payload = response.json()
            if response.status == 200 and payload.get("ready") is True:
                return
            last = f"HTTP {response.status}: {payload!r}"
        except Exception as exc:  # noqa: BLE001 - diagnostics are required
            last = str(exc)
        time.sleep(2)
    raise ContractFailure(f"API did not become ready: {last}")


def signup(email: str) -> str:
    payload = expect(
        call("POST", "/api/auth/signup", payload={"email": email, "password": PASSWORD}),
        201,
        f"signup {email}",
    )
    token = str(payload.get("token", ""))
    require(token.startswith("token-"), f"signup did not return a bearer token: {payload!r}")
    return token


def login(email: str) -> str:
    payload = expect(
        call("POST", "/api/auth/login", payload={"email": email, "password": PASSWORD}),
        200,
        f"login {email}",
    )
    token = str(payload.get("token", ""))
    require(token.startswith("token-"), f"login did not return a bearer token: {payload!r}")
    return token


def player(player_id: str, name: str, position: str) -> dict[str, Any]:
    return {
        "id": player_id,
        "playerId": player_id,
        "name": name,
        "position": position,
        "team": "Pilot State",
        "conference": "Test",
        "projection": 10.0,
    }


def roster_player_ids(state: dict[str, Any]) -> list[str]:
    return [str(item.get("id", item.get("playerId", ""))) for item in state.get("roster", [])]


def offer_by_id(state: dict[str, Any], trade_id: str) -> dict[str, Any]:
    for offer in state.get("offers", []):
        if str(offer.get("id", "")) == trade_id:
            return offer
    raise ContractFailure(f"trade {trade_id} missing from state: {state!r}")


def claim_by_id(state: dict[str, Any], claim_id: str) -> dict[str, Any]:
    for claim in state.get("claims", []):
        if str(claim.get("id", "")) == claim_id:
            return claim
    raise ContractFailure(f"waiver claim {claim_id} missing from state: {state!r}")


def trade_transaction(
    league_id: str,
    token: str,
    key: str,
    action: str,
    version: int,
    **details: Any,
) -> Response:
    return call(
        "POST",
        f"/api/leagues/{league_id}/trades/transactions",
        token=token,
        operation_key=key,
        payload={"action": action, "expectedVersion": version, **details},
    )


def waiver_transaction(
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


def scoring_transaction(
    league_id: str,
    token: str,
    key: str,
    action: str,
    version: int,
) -> Response:
    return call(
        "POST",
        f"/api/leagues/{league_id}/scoring/transactions",
        token=token,
        operation_key=key,
        payload={
            "action": action,
            "season": SEASON,
            "week": 1,
            "expectedVersion": version,
        },
    )


def activate_joined_manager(league_id: str, owner_token: str, email: str, team_name: str) -> None:
    expect(
        call(
            "PUT",
            f"/api/leagues/{league_id}/members/{encoded(email)}",
            token=owner_token,
            payload={"role": "member", "status": "Active", "teamName": team_name},
        ),
        200,
        f"activate {email}",
    )


def seed_scoring_stats(league_id: str, emails: list[str]) -> dict[str, str]:
    quarterbacks: dict[str, str] = {}
    digest = hashlib.sha256(RUN_KEY.encode()).hexdigest()
    game_id = int(digest[:12], 16) % 900_000_000_000 + 100_000_000_000
    with psycopg.connect(DB_URL) as connection:
        with connection.cursor() as cursor:
            cursor.execute(
                "INSERT INTO games (id, season, week, season_type, updated_at) "
                "VALUES (%s, %s, 1, 'regular', NOW()) "
                "ON CONFLICT (id) DO UPDATE SET season = EXCLUDED.season, week = 1, updated_at = NOW()",
                (game_id, SEASON),
            )
            for index, email in enumerate(emails, start=1):
                cursor.execute(
                    "SELECT player_id, player_snapshot FROM rosters "
                    "WHERE league_id = %s AND lower(manager_email) = lower(%s) "
                    "AND lower(roster_slot) = 'qb' LIMIT 1",
                    (league_id, email),
                )
                row = cursor.fetchone()
                require(row is not None, f"manager {email} has no quarterback starter")
                player_id = str(row[0])
                snapshot = row[1] if isinstance(row[1], dict) else json.loads(str(row[1]))
                quarterbacks[email] = player_id
                cursor.execute(
                    "INSERT INTO players (id, full_name, position, team, updated_at, raw) "
                    "VALUES (%s, %s, 'QB', 'Pilot State', NOW(), %s::jsonb) "
                    "ON CONFLICT (id) DO UPDATE SET full_name = EXCLUDED.full_name, "
                    "position = 'QB', team = 'Pilot State', updated_at = NOW(), raw = EXCLUDED.raw",
                    (player_id, snapshot.get("name", player_id), json.dumps(snapshot)),
                )
                cursor.execute(
                    "INSERT INTO player_stats "
                    "(player_id, season, week, team, conference, category, stat_name, stat_value, game_id, updated_at) "
                    "VALUES (%s, %s, 1, 'Pilot State', 'Test', 'passing', 'passingYards', %s, %s, NOW()) "
                    "ON CONFLICT (player_id, season, week, category, stat_name, game_id) "
                    "DO UPDATE SET stat_value = EXCLUDED.stat_value, updated_at = NOW()",
                    (player_id, SEASON, index * 100, game_id),
                )
        connection.commit()
    return quarterbacks


def database_invariants(league_id: str, expected_players: int) -> dict[str, int]:
    with psycopg.connect(DB_URL) as connection:
        with connection.cursor() as cursor:
            cursor.execute(
                "SELECT COUNT(*), COUNT(DISTINCT player_id) FROM rosters WHERE league_id = %s",
                (league_id,),
            )
            roster_count, unique_count = (int(value) for value in cursor.fetchone())
            cursor.execute(
                "SELECT COUNT(*) FROM draft_picks WHERE league_id = %s",
                (league_id,),
            )
            draft_count = int(cursor.fetchone()[0])
            cursor.execute(
                "SELECT COUNT(*) FROM league_standings WHERE league_id = %s AND season = %s",
                (league_id, SEASON),
            )
            standings_count = int(cursor.fetchone()[0])
            cursor.execute(
                "SELECT COUNT(*) FROM transactions WHERE league_id = %s",
                (league_id,),
            )
            transaction_count = int(cursor.fetchone()[0])
    require(roster_count == expected_players, f"expected {expected_players} roster rows, found {roster_count}")
    require(unique_count == expected_players, "a player is owned by more than one manager")
    require(draft_count == 8, f"expected eight draft picks, found {draft_count}")
    require(standings_count == 4, f"expected four materialized standings rows, found {standings_count}")
    return {
        "rosterRows": roster_count,
        "uniquePlayers": unique_count,
        "draftPicks": draft_count,
        "standingsRows": standings_count,
        "transactions": transaction_count,
    }


def main() -> None:
    wait_for_api()
    emails = [
        f"pilot-owner-{RUN_KEY}@example.test",
        f"pilot-manager-a-{RUN_KEY}@example.test",
        f"pilot-manager-b-{RUN_KEY}@example.test",
        f"pilot-manager-c-{RUN_KEY}@example.test",
    ]
    team_names = ["Pilot Alpha", "Pilot Bravo", "Pilot Charlie", "Pilot Delta"]
    tokens = {email: signup(email) for email in emails}
    owner = emails[0]

    created = expect(
        call(
            "POST",
            "/api/leagues",
            token=tokens[owner],
            operation_key=f"pilot-create-{RUN_KEY}",
            payload={
                "name": f"Beta Pilot {RUN_KEY}",
                "teams": 4,
                "scoring": "ppr",
                "draftType": "snake",
                "draftLobbyOpen": True,
                "invitedEmails": emails[1:],
                "rosterRules": {"qb": 1, "rb": 0, "wr": 0, "te": 0, "flex": 0, "bench": 1},
                "waiverRules": {
                    "mode": "waivers",
                    "claimDeadline": "2000-01-01T00:00",
                    "freeAgencyLocked": True,
                },
                "tradeRules": {"commissionerApproval": False, "expirationHours": 48},
                "notes": "deterministic exact-commit beta pilot",
            },
        ),
        201,
        "create pilot league",
    )
    league_id = str(created.get("id", ""))
    require(league_id, f"pilot league ID missing: {created!r}")

    isolation = expect(
        call(
            "POST",
            "/api/leagues",
            token=tokens[emails[3]],
            operation_key=f"pilot-isolation-{RUN_KEY}",
            payload={
                "name": f"Beta Pilot Isolation {RUN_KEY}",
                "teams": 4,
                "scoring": "ppr",
                "draftType": "snake",
                "invitedEmails": [],
                "rosterRules": {"qb": 1, "rb": 0, "wr": 0, "te": 0, "flex": 0, "bench": 1},
            },
        ),
        201,
        "create isolation league",
    )
    isolation_id = str(isolation.get("id", ""))
    expect(
        call("GET", f"/api/leagues/{isolation_id}", token=tokens[owner]),
        404,
        "cross-league isolation",
    )

    expect(
        call(
            "PUT",
            f"/api/leagues/{league_id}/team-name",
            token=tokens[owner],
            payload={"teamName": team_names[0]},
        ),
        200,
        "owner team name",
    )
    for index, email in enumerate(emails[1:], start=1):
        joined = expect(
            call("POST", f"/api/leagues/{league_id}/join", token=tokens[email]),
            (200, 202),
            f"join {email}",
        )
        if str(joined.get("joinStatus", "")).lower() == "pending_approval":
            activate_joined_manager(league_id, tokens[owner], email, team_names[index])
        else:
            expect(
                call(
                    "PUT",
                    f"/api/leagues/{league_id}/team-name",
                    token=tokens[email],
                    payload={"teamName": team_names[index]},
                ),
                200,
                f"team name {email}",
            )

    members = expect(
        call("GET", f"/api/leagues/{league_id}/members", token=tokens[owner]),
        200,
        "load active managers",
    )
    active = {
        str(member.get("email", "")).lower(): str(member.get("teamName", ""))
        for member in members
        if str(member.get("status", "")).lower() == "active"
    }
    require(set(emails).issubset(active), f"not all pilot managers are active: {active!r}")
    require(len({active[email] for email in emails}) == 4, f"team names are not unique: {active!r}")

    initial_draft = expect(
        call("GET", f"/api/leagues/{league_id}/draft", token=tokens[owner]),
        200,
        "initial draft state",
    )
    order = expect(
        call(
            "PUT",
            f"/api/leagues/{league_id}/draft/order",
            token=tokens[owner],
            operation_key=f"pilot-order-{RUN_KEY}",
            payload={"draftOrder": emails, "expectedVersion": int(initial_draft.get("version", 0))},
        ),
        200,
        "save deterministic draft order",
    )
    require(order.get("draftOrder") == emails, f"draft order did not persist: {order!r}")

    ready_state = order
    for email in emails:
        ready_state = expect(
            call(
                "POST",
                f"/api/leagues/{league_id}/draft/readiness",
                token=tokens[email],
                payload={"ready": True},
            ),
            200,
            f"ready {email}",
        )
    require(ready_state.get("allReady") is True, f"draft did not observe all managers ready: {ready_state!r}")

    draft = expect(
        call(
            "POST",
            f"/api/leagues/{league_id}/draft/start",
            token=tokens[owner],
            operation_key=f"pilot-start-{RUN_KEY}",
            payload={"expectedVersion": int(ready_state["version"]), "force": False},
        ),
        200,
        "start pilot draft",
    )
    require(draft.get("status") == "open", f"pilot draft did not open: {draft!r}")
    draft_players = [
        player(f"pilot-qb-{index}-{RUN_KEY}", f"Pilot Quarterback {index}", "QB")
        for index in range(1, 5)
    ] + [
        player(f"pilot-wr-{index}-{RUN_KEY}", f"Pilot Receiver {index}", "WR")
        for index in range(1, 5)
    ]

    first_pick_key = f"pilot-pick-1-{RUN_KEY}"
    first_payload: dict[str, Any] | None = None
    for index, selected in enumerate(draft_players, start=1):
        current_manager = str(draft.get("currentManager", "")).lower()
        require(current_manager in tokens, f"pick {index} has unknown manager: {draft!r}")
        payload = {
            "expectedVersion": int(draft["version"]),
            "expectedPick": int(draft["currentPick"]),
            "player": selected,
        }
        operation_key = first_pick_key if index == 1 else f"pilot-pick-{index}-{RUN_KEY}"
        draft = expect(
            call(
                "POST",
                f"/api/leagues/{league_id}/draft/picks",
                token=tokens[current_manager],
                operation_key=operation_key,
                payload=payload,
            ),
            201,
            f"draft pick {index}",
        )
        if index == 1:
            first_payload = payload
            replay = expect(
                call(
                    "POST",
                    f"/api/leagues/{league_id}/draft/picks",
                    token=tokens[current_manager],
                    operation_key=operation_key,
                    payload=payload,
                ),
                200,
                "replay first draft pick",
            )
            require(replay.get("idempotentReplay") is True, f"pick replay was not identified: {replay!r}")
            require(len(replay.get("picks", [])) == 1, f"pick replay duplicated the selection: {replay!r}")
    require(first_payload is not None, "first draft payload was not retained")
    require(draft.get("status") == "complete", f"eight-pick draft did not complete: {draft!r}")
    require(len(draft.get("picks", [])) == 8, f"draft pick count is not eight: {draft!r}")

    rosters: dict[str, dict[str, Any]] = {}
    all_rostered: list[str] = []
    for email in emails:
        state = expect(
            call("GET", f"/api/leagues/{league_id}/roster/state", token=tokens[email]),
            200,
            f"roster state {email}",
        )
        ids = roster_player_ids(state)
        require(len(ids) == 2, f"manager {email} does not have two drafted players: {state!r}")
        require(
            sorted(str(item.get("rosterSlot", "")).lower() for item in state.get("roster", [])) == ["bench", "qb"],
            f"manager {email} roster slots are not QB plus bench: {state!r}",
        )
        all_rostered.extend(ids)
        rosters[email] = state
    require(len(all_rostered) == len(set(all_rostered)) == 8, f"draft ownership is not unique: {all_rostered!r}")

    trade_state = expect(
        call("GET", f"/api/leagues/{league_id}/trades/state", token=tokens[owner]),
        200,
        "initial trade state",
    )
    offer_players = rosters[owner]["roster"]
    request_players = rosters[emails[1]]["roster"]
    created_trade = expect(
        trade_transaction(
            league_id,
            tokens[owner],
            f"pilot-trade-create-{RUN_KEY}",
            "create",
            int(trade_state["version"]),
            offerPlayers=offer_players,
            requestPlayers=request_players,
            targetManager=emails[1],
            note="deterministic two-for-two pilot trade",
        ),
        200,
        "create two-for-two trade",
    )
    trade_id = str(created_trade.get("tradeId", ""))
    require(trade_id, f"trade ID missing: {created_trade!r}")
    accepted_trade = expect(
        trade_transaction(
            league_id,
            tokens[emails[1]],
            f"pilot-trade-accept-{RUN_KEY}",
            "status",
            int(created_trade["version"]),
            tradeId=trade_id,
            status="Accepted",
        ),
        200,
        "accept two-for-two trade",
    )
    require(offer_by_id(accepted_trade, trade_id).get("status") == "Approved", f"trade did not execute: {accepted_trade!r}")
    owner_after_trade = expect(
        call("GET", f"/api/leagues/{league_id}/roster/state", token=tokens[owner]),
        200,
        "owner roster after trade",
    )
    manager_after_trade = expect(
        call("GET", f"/api/leagues/{league_id}/roster/state", token=tokens[emails[1]]),
        200,
        "manager roster after trade",
    )
    require(set(roster_player_ids(owner_after_trade)) == set(roster_player_ids(rosters[emails[1]])), "owner did not receive full requested package")
    require(set(roster_player_ids(manager_after_trade)) == set(roster_player_ids(rosters[owner])), "manager did not receive full offered package")

    waiver_manager = emails[2]
    waiver_roster = rosters[waiver_manager]
    drop_player = next(
        item for item in waiver_roster["roster"]
        if str(item.get("rosterSlot", "")).lower() == "bench"
    )
    waiver_add = player(f"pilot-waiver-wr-{RUN_KEY}", "Pilot Waiver Receiver", "WR")
    waiver_state = expect(
        call("GET", f"/api/leagues/{league_id}/waivers/state", token=tokens[waiver_manager]),
        200,
        "initial waiver state",
    )
    created_claim = expect(
        waiver_transaction(
            league_id,
            tokens[waiver_manager],
            f"pilot-waiver-create-{RUN_KEY}",
            "create",
            int(waiver_state["version"]),
            addPlayer=waiver_add,
            dropPlayerId=str(drop_player.get("id", "")),
        ),
        200,
        "create pilot waiver claim",
    )
    claim_id = str(created_claim.get("claimId", ""))
    require(claim_id, f"waiver claim ID missing: {created_claim!r}")
    processed = expect(
        waiver_transaction(
            league_id,
            tokens[owner],
            f"pilot-waiver-process-{RUN_KEY}",
            "process",
            int(created_claim["version"]),
        ),
        200,
        "process pilot waiver claim",
    )
    final_claim = claim_by_id(processed, claim_id)
    require(final_claim.get("status") == "Processed", f"waiver claim did not process: {final_claim!r}")
    waiver_after = expect(
        call("GET", f"/api/leagues/{league_id}/roster/state", token=tokens[waiver_manager]),
        200,
        "waiver manager roster after processing",
    )
    waiver_ids = roster_player_ids(waiver_after)
    require(waiver_add["id"] in waiver_ids, f"waiver addition missing: {waiver_after!r}")
    require(str(drop_player.get("id")) not in waiver_ids, f"waiver drop still rostered: {waiver_after!r}")

    quarterbacks = seed_scoring_stats(league_id, emails)
    score_state = expect(
        call(
            "GET",
            f"/api/leagues/{league_id}/scoring/state?season={SEASON}&week=1",
            token=tokens[owner],
        ),
        200,
        "initial scoring state",
    )
    scored = expect(
        scoring_transaction(
            league_id,
            tokens[owner],
            f"pilot-score-{RUN_KEY}",
            "score",
            int(score_state["version"]),
        ),
        200,
        "score pilot week",
    )
    require(scored.get("status") == "scored", f"week did not enter scored state: {scored!r}")
    points = sorted(round(float(item.get("fantasyPoints", 0)), 6) for item in scored.get("scores", []))
    require(points == [4.0, 8.0, 12.0, 16.0], f"unexpected pilot fantasy points: {points!r}")
    finalized = expect(
        scoring_transaction(
            league_id,
            tokens[owner],
            f"pilot-finalize-{RUN_KEY}",
            "finalize",
            int(scored["version"]),
        ),
        200,
        "finalize pilot week",
    )
    require(finalized.get("status") == "final", f"pilot week did not finalize: {finalized!r}")
    standings_payload = expect(
        call(
            "GET",
            f"/api/leagues/{league_id}/standings?season={SEASON}",
            token=tokens[owner],
        ),
        200,
        "load pilot standings",
    )
    standings = standings_payload.get("standings", [])
    require(len(standings) == 4, f"standings did not include four managers: {standings!r}")
    require(sum(int(row.get("gamesPlayed", 0)) for row in standings) == 4, f"standings counted games incorrectly: {standings!r}")

    transactions = expect(
        call("GET", f"/api/leagues/{league_id}/transactions", token=tokens[owner]),
        200,
        "load pilot audit trail",
    )
    audit_text = json.dumps(transactions).lower()
    for keyword in ("draft", "trade", "waiver", "scoring"):
        require(keyword in audit_text, f"audit trail is missing {keyword}: {transactions!r}")

    old_tokens = dict(tokens)
    for email in emails:
        expect(
            call("POST", "/api/auth/logout", token=tokens[email]),
            (200, 204),
            f"logout {email}",
        )
        validation = call("GET", "/api/auth/validate", token=old_tokens[email])
        validation_body = validation.json() or {}
        require(
            validation.status in (401, 403) or validation_body.get("valid") is not True,
            f"logout token remained valid for {email}: {validation.status} {validation_body!r}",
        )
        tokens[email] = login(email)

    for email in emails:
        leagues = expect(call("GET", "/api/leagues", token=tokens[email]), 200, f"reload leagues {email}")
        require(any(str(item.get("id", "")) == league_id for item in leagues), f"reloaded account {email} lost pilot league")
        restored_roster = expect(
            call("GET", f"/api/leagues/{league_id}/roster/state", token=tokens[email]),
            200,
            f"reload roster {email}",
        )
        require(len(restored_roster.get("roster", [])) == 2, f"reloaded roster wrong for {email}: {restored_roster!r}")

    restored_draft = expect(
        call("GET", f"/api/leagues/{league_id}/draft", token=tokens[owner]),
        200,
        "reload completed draft",
    )
    require(restored_draft.get("status") == "complete" and len(restored_draft.get("picks", [])) == 8, "completed draft did not persist")
    restored_trades = expect(
        call("GET", f"/api/leagues/{league_id}/trades/state", token=tokens[owner]),
        200,
        "reload completed trade",
    )
    require(offer_by_id(restored_trades, trade_id).get("status") == "Approved", "completed trade did not persist")
    restored_waivers = expect(
        call("GET", f"/api/leagues/{league_id}/waivers/state", token=tokens[owner]),
        200,
        "reload processed waiver",
    )
    require(claim_by_id(restored_waivers, claim_id).get("status") == "Processed", "processed waiver did not persist")
    restored_scoring = expect(
        call(
            "GET",
            f"/api/leagues/{league_id}/scoring/state?season={SEASON}&week=1",
            token=tokens[owner],
        ),
        200,
        "reload finalized scoring",
    )
    require(restored_scoring.get("status") == "final", "finalized scoring did not persist")
    restored_standings = expect(
        call(
            "GET",
            f"/api/leagues/{league_id}/standings?season={SEASON}",
            token=tokens[owner],
        ),
        200,
        "reload standings",
    ).get("standings", [])
    require(restored_standings == standings, "standings changed after logout/login reload")

    database = database_invariants(league_id, expected_players=8)
    report = {
        "status": "passed",
        "runKey": RUN_KEY,
        "leagueId": league_id,
        "accounts": 4,
        "draftPicks": 8,
        "tradePlayersPerSide": 2,
        "waiverClaim": claim_id,
        "quarterbacks": quarterbacks,
        "standings": standings,
        "database": database,
        "persistence": {
            "logout": True,
            "login": True,
            "league": True,
            "draft": True,
            "rosters": True,
            "trade": True,
            "waiver": True,
            "scoring": True,
            "standings": True,
        },
    }
    REPORT_PATH.parent.mkdir(parents=True, exist_ok=True)
    REPORT_PATH.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps(report, sort_keys=True))


if __name__ == "__main__":
    try:
        main()
    except ContractFailure as error:
        failure = {"status": "failed", "runKey": RUN_KEY, "error": str(error)}
        REPORT_PATH.parent.mkdir(parents=True, exist_ok=True)
        REPORT_PATH.write_text(json.dumps(failure, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(f"beta pilot lifecycle contract failure: {error}", flush=True)
        raise SystemExit(1) from error
