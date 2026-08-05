#!/usr/bin/env python3
"""Database-backed contracts for atomic, replay-safe trade processing."""
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
PASSWORD = os.getenv("CFF_CONTRACT_PASSWORD", "Trade-Contract-Password-2026!")
RUN_KEY = os.getenv("CFF_TRADE_RUN_KEY", str(time.time_ns()))


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


def player(player_id: str, name: str, position: str) -> dict[str, Any]:
    return {
        "id": player_id,
        "playerId": player_id,
        "name": name,
        "position": position,
        "team": "Contract U",
        "projection": 10.0,
    }


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
        f"/api/leagues/{league_id}/trades/transactions",
        token=token,
        operation_key=key,
        payload={"action": action, "expectedVersion": version, **details},
    )


def offer_by_id(state: dict[str, Any], trade_id: str) -> dict[str, Any]:
    for offer in state.get("offers", []):
        if str(offer.get("id", "")) == trade_id:
            return offer
    raise ContractFailure(f"trade {trade_id} missing from state: {state!r}")


def configure_league(
    league_id: str,
    emails: list[str],
    players: dict[str, tuple[str, dict[str, Any], str]],
) -> None:
    with psycopg.connect(DB_URL) as connection:
        with connection.cursor() as cursor:
            cursor.execute(
                "UPDATE league_members SET status = 'active', joined_at = COALESCE(joined_at, NOW()), "
                "updated_at = NOW() WHERE league_id = %s",
                (league_id,),
            )
            cursor.execute(
                "UPDATE leagues SET trade_rules = %s::jsonb, updated_at = NOW() WHERE id = %s",
                (json.dumps({"commissionerApproval": False, "expirationHours": 48}), league_id),
            )
            cursor.execute("DELETE FROM rosters WHERE league_id = %s", (league_id,))
            for player_id, (manager, snapshot, slot) in players.items():
                cursor.execute(
                    "INSERT INTO rosters "
                    "(league_id, manager_email, player_id, player_snapshot, roster_slot, acquired_via, acquired_at) "
                    "VALUES (%s, %s, %s, %s::jsonb, %s, 'draft', NOW())",
                    (league_id, manager, player_id, json.dumps(snapshot), slot),
                )
            cursor.execute(
                "INSERT INTO roster_states (league_id, manager_email, version, updated_at) "
                "SELECT %s, lower(email), 0, NOW() FROM league_members WHERE league_id = %s "
                "ON CONFLICT (league_id, manager_email) DO UPDATE SET version = 0, updated_at = NOW()",
                (league_id, league_id),
            )
            cursor.execute(
                "INSERT INTO trade_states (league_id, version, updated_at) VALUES (%s, 0, NOW()) "
                "ON CONFLICT (league_id) DO UPDATE SET version = 0, updated_at = NOW()",
                (league_id,),
            )
            cursor.execute("DELETE FROM trade_operations WHERE league_id = %s", (league_id,))
            cursor.execute("DELETE FROM trade_player_locks WHERE league_id = %s", (league_id,))
            cursor.execute("DELETE FROM trade_offers WHERE league_id = %s", (league_id,))
            cursor.execute(
                "SELECT COUNT(*) FROM league_members WHERE league_id = %s AND status = 'active'",
                (league_id,),
            )
            require(int(cursor.fetchone()[0]) == len(emails), "not all invited managers became active")
        connection.commit()


def set_approval_rule(league_id: str, required: bool) -> None:
    with psycopg.connect(DB_URL) as connection:
        with connection.cursor() as cursor:
            cursor.execute(
                "UPDATE leagues SET trade_rules = %s::jsonb, updated_at = NOW() WHERE id = %s",
                (json.dumps({"commissionerApproval": required, "expirationHours": 48}), league_id),
            )
        connection.commit()


def set_expired(trade_id: str) -> None:
    with psycopg.connect(DB_URL) as connection:
        with connection.cursor() as cursor:
            cursor.execute(
                "UPDATE trade_offers SET expires_at = NOW() - INTERVAL '1 minute' WHERE id = %s",
                (trade_id,),
            )
        connection.commit()


def owner(league_id: str, player_id: str) -> str:
    with psycopg.connect(DB_URL) as connection:
        with connection.cursor() as cursor:
            cursor.execute(
                "SELECT lower(manager_email) FROM rosters WHERE league_id = %s AND player_id = %s",
                (league_id, player_id),
            )
            rows = cursor.fetchall()
            require(len(rows) <= 1, f"player {player_id} has duplicate owners: {rows!r}")
            return str(rows[0][0]) if rows else ""


def lock_count(league_id: str, trade_id: str) -> int:
    with psycopg.connect(DB_URL) as connection:
        with connection.cursor() as cursor:
            cursor.execute(
                "SELECT COUNT(*) FROM trade_player_locks WHERE league_id = %s AND offer_id = %s",
                (league_id, trade_id),
            )
            return int(cursor.fetchone()[0])


def operation_count(league_id: str, email: str, key: str) -> int:
    with psycopg.connect(DB_URL) as connection:
        with connection.cursor() as cursor:
            cursor.execute(
                "SELECT COUNT(*) FROM trade_operations "
                "WHERE league_id = %s AND lower(actor_email) = lower(%s) AND operation_key = %s",
                (league_id, email, key),
            )
            return int(cursor.fetchone()[0])


def roster_version(league_id: str, email: str) -> int:
    with psycopg.connect(DB_URL) as connection:
        with connection.cursor() as cursor:
            cursor.execute(
                "SELECT version FROM roster_states WHERE league_id = %s AND lower(manager_email) = lower(%s)",
                (league_id, email),
            )
            row = cursor.fetchone()
            return int(row[0]) if row else 0


def main() -> None:
    wait_for_api()
    emails = [
        f"trade-commissioner-{RUN_KEY}@example.test",
        f"trade-offerer-{RUN_KEY}@example.test",
        f"trade-recipient-{RUN_KEY}@example.test",
        f"trade-member-{RUN_KEY}@example.test",
    ]
    tokens = {email: signup(email) for email in emails}
    commissioner, offerer, recipient, member = emails

    created = expect(
        call(
            "POST",
            "/api/leagues",
            token=tokens[commissioner],
            operation_key=f"create-{RUN_KEY}",
            payload={
                "name": f"Trade Contract {RUN_KEY}",
                "teams": 4,
                "scoring": "ppr",
                "draftType": "snake",
                "invitedEmails": emails[1:],
                "rosterRules": {"qb": 1, "rb": 2, "wr": 2, "te": 1, "flex": 2, "bench": 6},
            },
        ),
        201,
        "create trade league",
    )
    league_id = str(created.get("id", ""))
    require(league_id, f"created league ID missing: {created!r}")

    a = player(f"trade-a-{RUN_KEY}", "Trade A", "WR")
    b = player(f"trade-b-{RUN_KEY}", "Trade B", "RB")
    c = player(f"trade-c-{RUN_KEY}", "Trade C", "QB")
    d = player(f"trade-d-{RUN_KEY}", "Trade D", "TE")
    e = player(f"trade-e-{RUN_KEY}", "Trade E", "WR")
    f = player(f"trade-f-{RUN_KEY}", "Trade F", "RB")
    players = {
        a["id"]: (offerer, a, "wr"),
        b["id"]: (recipient, b, "rb"),
        c["id"]: (offerer, c, "qb"),
        d["id"]: (recipient, d, "te"),
        e["id"]: (offerer, e, "wr"),
        f["id"]: (recipient, f, "rb"),
    }
    configure_league(league_id, emails, players)

    initial = expect(
        call("GET", f"/api/leagues/{league_id}/trades/state", token=tokens[offerer]),
        200,
        "initial trade state",
    )
    require(initial.get("version") == 0, f"initial trade version wrong: {initial!r}")

    direct_key = f"direct-create-{RUN_KEY}"
    direct = expect(
        transaction(
            league_id,
            tokens[offerer],
            direct_key,
            "create",
            0,
            offerPlayer=a,
            requestPlayer=b,
            requestPlayerName=b["name"],
            targetManager=recipient,
            note="direct race",
        ),
        200,
        "create direct trade",
    )
    direct_id = str(direct["tradeId"])
    direct_version = int(direct["version"])
    require(lock_count(league_id, direct_id) == 2, "direct trade did not lock both players")

    locked = transaction(
        league_id,
        tokens[offerer],
        f"locked-create-{RUN_KEY}",
        "create",
        direct_version,
        offerPlayer=a,
        requestPlayer=d,
        requestPlayerName=d["name"],
        targetManager=recipient,
    )
    locked_body = locked.json()
    require(locked.status == 409 and locked_body.get("code") == "trade_player_locked",
            f"open trade player lock was not enforced: {locked.status} {locked_body!r}")

    accept_key = f"race-accept-{RUN_KEY}"
    cancel_key = f"race-cancel-{RUN_KEY}"

    def accept_direct() -> Response:
        return transaction(
            league_id, tokens[recipient], accept_key, "status", direct_version,
            tradeId=direct_id, status="Accepted",
        )

    def cancel_direct() -> Response:
        return transaction(
            league_id, tokens[offerer], cancel_key, "status", direct_version,
            tradeId=direct_id, status="Cancelled",
        )

    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
        accept_future = executor.submit(accept_direct)
        cancel_future = executor.submit(cancel_direct)
        accept_response = accept_future.result(timeout=30)
        cancel_response = cancel_future.result(timeout=30)

    require(sorted([accept_response.status, cancel_response.status]) == [200, 409],
            f"accept/cancel race did not produce one winner: {accept_response.status}, {cancel_response.status}")
    winner_response = accept_response if accept_response.status == 200 else cancel_response
    winner_key = accept_key if accept_response.status == 200 else cancel_key
    winner_email = recipient if accept_response.status == 200 else offerer
    winner_payload = winner_response.json()
    terminal = offer_by_id(winner_payload, direct_id)
    require(terminal["status"] in {"Approved", "Cancelled"}, f"unexpected race terminal state: {terminal!r}")
    require(lock_count(league_id, direct_id) == 0, "terminal direct trade retained player locks")
    if terminal["status"] == "Approved":
        require(owner(league_id, a["id"]) == recipient, "accepted direct trade did not move offered player")
        require(owner(league_id, b["id"]) == offerer, "accepted direct trade did not move requested player")
    else:
        require(owner(league_id, a["id"]) == offerer, "cancelled direct trade changed offered ownership")
        require(owner(league_id, b["id"]) == recipient, "cancelled direct trade changed requested ownership")
    require(operation_count(league_id, winner_email, winner_key) == 1, "race winner operation not recorded exactly once")

    replay = accept_direct() if accept_response.status == 200 else cancel_direct()
    replay_body = expect(replay, 200, "race winner replay")
    require(replay_body.get("idempotentReplay") is True, f"winner replay was not identified: {replay_body!r}")
    require(int(replay_body["version"]) == int(winner_payload["version"]), "winner replay advanced trade version")

    set_approval_rule(league_id, True)
    approval_state = expect(
        call("GET", f"/api/leagues/{league_id}/trades/state", token=tokens[offerer]),
        200,
        "approval rule state",
    )
    approval_create = expect(
        transaction(
            league_id,
            tokens[offerer],
            f"approval-create-{RUN_KEY}",
            "create",
            int(approval_state["version"]),
            offerPlayer=c,
            requestPlayer=d,
            requestPlayerName=d["name"],
            targetManager=recipient,
        ),
        200,
        "create approval trade",
    )
    approval_id = str(approval_create["tradeId"])
    approval_accept = expect(
        transaction(
            league_id,
            tokens[recipient],
            f"approval-accept-{RUN_KEY}",
            "status",
            int(approval_create["version"]),
            tradeId=approval_id,
            status="Accepted",
        ),
        200,
        "recipient accepts approval trade",
    )
    accepted_offer = offer_by_id(approval_accept, approval_id)
    require(accepted_offer["status"] == "Accepted", f"approval trade executed before approval: {accepted_offer!r}")
    require(owner(league_id, c["id"]) == offerer and owner(league_id, d["id"]) == recipient,
            "accepted approval trade moved players early")
    require(lock_count(league_id, approval_id) == 2, "accepted approval trade released locks early")

    offered_before = roster_version(league_id, offerer)
    recipient_before = roster_version(league_id, recipient)
    approve_key = f"approval-final-{RUN_KEY}"
    approval = expect(
        transaction(
            league_id,
            tokens[commissioner],
            approve_key,
            "status",
            int(approval_accept["version"]),
            tradeId=approval_id,
            status="Approved",
        ),
        200,
        "commissioner approves trade",
    )
    approved_offer = offer_by_id(approval, approval_id)
    require(approved_offer["status"] == "Approved", f"commissioner approval did not close trade: {approved_offer!r}")
    require(owner(league_id, c["id"]) == recipient, "approved trade did not move offered player")
    require(owner(league_id, d["id"]) == offerer, "approved trade did not move requested player")
    require(lock_count(league_id, approval_id) == 0, "approved trade retained player locks")
    require(roster_version(league_id, offerer) == offered_before + 1, "offerer roster version did not advance once")
    require(roster_version(league_id, recipient) == recipient_before + 1, "recipient roster version did not advance once")

    approval_replay = expect(
        transaction(
            league_id,
            tokens[commissioner],
            approve_key,
            "status",
            int(approval_accept["version"]),
            tradeId=approval_id,
            status="Approved",
        ),
        200,
        "commissioner approval replay",
    )
    require(approval_replay.get("idempotentReplay") is True, "approval replay was not identified")
    require(roster_version(league_id, offerer) == offered_before + 1, "approval replay advanced offerer roster twice")
    require(operation_count(league_id, commissioner, approve_key) == 1, "approval operation recorded more than once")

    set_approval_rule(league_id, False)
    expiry_state = expect(
        call("GET", f"/api/leagues/{league_id}/trades/state", token=tokens[offerer]),
        200,
        "pre-expiry state",
    )
    expiry_create = expect(
        transaction(
            league_id,
            tokens[offerer],
            f"expiry-create-{RUN_KEY}",
            "create",
            int(expiry_state["version"]),
            offerPlayer=e,
            requestPlayer=f,
            requestPlayerName=f["name"],
            targetManager=recipient,
        ),
        200,
        "create expiring trade",
    )
    expiry_id = str(expiry_create["tradeId"])
    set_expired(expiry_id)
    expired_state = expect(
        call("GET", f"/api/leagues/{league_id}/trades/state", token=tokens[offerer]),
        200,
        "expire trade on authoritative read",
    )
    expired_offer = offer_by_id(expired_state, expiry_id)
    require(expired_offer["status"] == "Expired", f"past-due trade was not expired: {expired_offer!r}")
    require(int(expired_state["version"]) == int(expiry_create["version"]) + 1,
            "expiration did not advance trade version exactly once")
    require(lock_count(league_id, expiry_id) == 0, "expired trade retained player locks")
    require(owner(league_id, e["id"]) == offerer and owner(league_id, f["id"]) == recipient,
            "expiration changed roster ownership")

    replacement = expect(
        transaction(
            league_id,
            tokens[offerer],
            f"replacement-create-{RUN_KEY}",
            "create",
            int(expired_state["version"]),
            offerPlayer=e,
            requestPlayer=f,
            requestPlayerName=f["name"],
            targetManager=recipient,
        ),
        200,
        "create replacement after expiration",
    )
    replacement_id = str(replacement["tradeId"])
    stale = transaction(
        league_id,
        tokens[recipient],
        f"stale-status-{RUN_KEY}",
        "status",
        int(expired_state["version"]),
        tradeId=replacement_id,
        status="Accepted",
    )
    stale_body = stale.json()
    require(stale.status == 409 and stale_body.get("code") == "trade_state_conflict",
            f"stale trade mutation was not rejected: {stale.status} {stale_body!r}")
    require(int(stale_body.get("currentVersion", -1)) == int(replacement["version"]),
            f"stale conflict omitted current version: {stale_body!r}")

    cleanup = expect(
        transaction(
            league_id,
            tokens[offerer],
            f"replacement-cancel-{RUN_KEY}",
            "status",
            int(replacement["version"]),
            tradeId=replacement_id,
            status="Cancelled",
        ),
        200,
        "cancel replacement trade",
    )
    require(offer_by_id(cleanup, replacement_id)["status"] == "Cancelled", "replacement cleanup failed")
    require(lock_count(league_id, replacement_id) == 0, "cancelled replacement retained locks")

    print("trade lifecycle runtime contracts passed")


if __name__ == "__main__":
    main()
