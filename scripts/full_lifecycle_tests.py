#!/usr/bin/env python3
"""Run a destructive three-user fantasy lifecycle against disposable staging."""
from __future__ import annotations

import json
import os
import time
import urllib.parse
from dataclasses import dataclass
from datetime import datetime, timezone
from typing import Any

from release_gate_common import (
    GateFailure,
    JsonHttpClient,
    add_check,
    enforce_checks,
    env_flag,
    mask_email,
    require_env,
    utc_now,
    write_failure_report,
    write_report,
)


@dataclass
class Account:
    email: str
    password: str
    token: str = ""


def encoded(value: str) -> str:
    return urllib.parse.quote(value, safe="")


def player_ids(roster: Any) -> set[str]:
    if not isinstance(roster, list):
        return set()
    return {str(player.get("id")) for player in roster if isinstance(player, dict) and player.get("id")}


def derive_standings(matchups: list[dict[str, Any]], members: list[str]) -> list[dict[str, Any]]:
    board = {
        email: {
            "managerEmail": email,
            "wins": 0,
            "losses": 0,
            "ties": 0,
            "pointsFor": 0.0,
            "pointsAgainst": 0.0,
        }
        for email in members
    }
    for matchup in matchups:
        if str(matchup.get("status", "")).lower() != "final":
            continue
        home = str(matchup.get("homeManager", ""))
        away = str(matchup.get("awayManager", ""))
        home_score = float(matchup.get("homeScore", 0) or 0)
        away_score = float(matchup.get("awayScore", 0) or 0)
        if home in board:
            board[home]["pointsFor"] += home_score
            board[home]["pointsAgainst"] += away_score
        if away in board:
            board[away]["pointsFor"] += away_score
            board[away]["pointsAgainst"] += home_score
        if not away:
            continue
        if home_score > away_score:
            board[home]["wins"] += 1
            board[away]["losses"] += 1
        elif away_score > home_score:
            board[away]["wins"] += 1
            board[home]["losses"] += 1
        else:
            board[home]["ties"] += 1
            board[away]["ties"] += 1
    return sorted(
        board.values(),
        key=lambda row: (-row["wins"], -row["ties"], -row["pointsFor"], row["managerEmail"]),
    )


def login(client: JsonHttpClient, account: Account) -> None:
    payload = client.request(
        "POST",
        "/api/auth/login",
        body={"email": account.email, "password": account.password},
    ).payload
    account.token = str(payload.get("token", ""))
    if not account.token:
        raise GateFailure(f"Login did not return a token for {mask_email(account.email)}")


def create_league(client: JsonHttpClient, owner: Account, payload: dict[str, Any]) -> dict[str, Any]:
    league = client.request("POST", "/api/leagues", token=owner.token, body=payload, expected=(201,)).payload
    if not isinstance(league, dict) or not league.get("id"):
        raise GateFailure(f"League creation did not return an id: {league}")
    return league


def main() -> int:
    base_url = require_env("CFF_API_BASE_URL")
    report_dir = os.environ.get("CFF_RELEASE_GATE_REPORT_DIR", "release-gate-artifacts")
    keep_resources = env_flag("CFF_LIFECYCLE_KEEP_RESOURCES", False)
    commissioner = Account(
        require_env("CFF_LIFECYCLE_COMMISSIONER_EMAIL").lower(),
        require_env("CFF_LIFECYCLE_COMMISSIONER_PASSWORD"),
    )
    manager_a = Account(
        require_env("CFF_LIFECYCLE_MANAGER_A_EMAIL").lower(),
        require_env("CFF_LIFECYCLE_MANAGER_A_PASSWORD"),
    )
    manager_b = Account(
        require_env("CFF_LIFECYCLE_MANAGER_B_EMAIL").lower(),
        require_env("CFF_LIFECYCLE_MANAGER_B_PASSWORD"),
    )
    accounts = (commissioner, manager_a, manager_b)
    if len({account.email for account in accounts}) != 3:
        raise GateFailure("Lifecycle accounts must use three distinct email addresses")

    client = JsonHttpClient(base_url, timeout=90)
    checks: list[dict[str, Any]] = []
    evidence: dict[str, Any] = {"accounts": [mask_email(account.email) for account in accounts]}
    league_id = ""
    isolation_id = ""
    failure: Exception | None = None

    try:
        health = client.request("GET", "/health").payload
        add_check(
            checks,
            "API and database healthy",
            health.get("status") == "ok" and health.get("database") == "ok",
            str(health),
        )
        for account in accounts:
            login(client, account)
        add_check(checks, "Three verified accounts can sign in", True, ", ".join(mask_email(a.email) for a in accounts))

        catalog = client.request("GET", "/api/players?limit=20&offset=0").payload
        unique_players: list[dict[str, Any]] = []
        seen: set[str] = set()
        for player in catalog if isinstance(catalog, list) else []:
            player_id = str(player.get("id", "")) if isinstance(player, dict) else ""
            if player_id and player_id not in seen:
                seen.add(player_id)
                unique_players.append(player)
        if len(unique_players) < 7:
            raise GateFailure(f"Lifecycle testing requires seven unique players; received {len(unique_players)}")
        add_check(checks, "Player catalog supports lifecycle", True, f"players={len(unique_players)}")

        suffix = int(time.time())
        league = create_league(
            client,
            commissioner,
            {
                "name": f"Release Gate League {suffix}",
                "teams": 4,
                "scoring": "ppr",
                "draftType": "snake",
                "draftLobbyOpen": True,
                "rosterRules": {"qb": 0, "rb": 0, "wr": 0, "te": 0, "flex": 0, "bench": 1},
                "waiverRules": {"mode": "waivers", "claimDeadline": "", "freeAgencyLocked": True},
                "tradeRules": {"commissionerApproval": False, "expirationHours": 48},
                "invitedEmails": [manager_a.email, manager_b.email],
                "notes": "release-gate-automation",
            },
        )
        league_id = str(league["id"])
        evidence["leagueId"] = league_id
        add_check(checks, "Commissioner creates release-gate league", True, league_id)

        isolation = create_league(
            client,
            manager_b,
            {
                "name": f"Release Gate Isolation {suffix}",
                "teams": 4,
                "scoring": "ppr",
                "draftType": "snake",
                "rosterRules": {"qb": 0, "rb": 0, "wr": 0, "te": 0, "flex": 0, "bench": 1},
                "invitedEmails": [],
                "notes": "release-gate-isolation",
            },
        )
        isolation_id = str(isolation["id"])
        client.request("GET", f"/api/leagues/{isolation_id}", token=commissioner.token, expected=(404,))
        add_check(checks, "Cross-league access is blocked", True, "nonmember received HTTP 404")

        for manager in (manager_a, manager_b):
            joined = client.request(
                "POST",
                f"/api/leagues/{league_id}/join",
                token=manager.token,
                expected=(200, 202),
            ).payload
            if joined.get("joinStatus") not in {"pending_approval", "active", None}:
                raise GateFailure(f"Unexpected join state for {mask_email(manager.email)}: {joined}")
            client.request(
                "PUT",
                f"/api/leagues/{league_id}/members/{encoded(manager.email)}",
                token=commissioner.token,
                body={"role": "member", "status": "Active", "teamName": f"Team {manager.email[:6]}"},
            )
        members = client.request("GET", f"/api/leagues/{league_id}/members", token=commissioner.token).payload
        active = {
            str(member.get("email", "")).lower()
            for member in members if isinstance(member, dict) and str(member.get("status", "")).lower() == "active"
        }
        add_check(checks, "Invites, joins, and approvals persist", {a.email for a in accounts}.issubset(active), str(sorted(active)))

        current_league = client.request("GET", f"/api/leagues/{league_id}", token=commissioner.token).payload
        client.request("PUT", f"/api/leagues/{league_id}", token=manager_a.token, body=current_league, expected=(403,))
        add_check(checks, "Commissioner-only settings enforced", True, "manager update returned HTTP 403")

        draft_order = [commissioner.email, manager_a.email, manager_b.email]
        order_state = client.request(
            "PUT",
            f"/api/leagues/{league_id}/draft/order",
            token=commissioner.token,
            body={"draftOrder": draft_order},
        ).payload
        add_check(checks, "Draft order saved", order_state.get("draftOrder") == draft_order, str(order_state.get("draftOrder")))
        queue_state = client.request(
            "PUT",
            f"/api/leagues/{league_id}/draft/queue",
            token=manager_a.token,
            body={"queue": [unique_players[1], unique_players[4]]},
        ).payload
        add_check(checks, "Draft queue saved", len(queue_state.get("queue", [])) == 2, str(queue_state.get("queue", [])))

        state: dict[str, Any] = {}
        for index, account in enumerate(accounts):
            state = client.request(
                "POST",
                f"/api/leagues/{league_id}/draft/picks",
                token=account.token,
                body={"player": unique_players[index]},
                expected=(201,),
            ).payload
        add_check(
            checks,
            "One-player snake draft completes",
            state.get("status") == "complete" and len(state.get("picks", [])) == 3,
            f"status={state.get('status')}; picks={len(state.get('picks', []))}",
        )
        queue = client.request("GET", f"/api/leagues/{league_id}/draft", token=manager_a.token).payload.get("queue", [])
        add_check(checks, "Draft queue persists", str(unique_players[4]["id"]) in player_ids(queue), str(queue))
        client.request("POST", f"/api/leagues/{league_id}/draft/undo", token=manager_a.token, expected=(403,))
        add_check(checks, "Draft undo is commissioner-only", True, "manager undo returned HTTP 403")

        rosters = {
            account.email: client.request("GET", f"/api/leagues/{league_id}/roster", token=account.token).payload
            for account in accounts
        }
        add_check(checks, "Drafted rosters persisted", all(len(roster) == 1 for roster in rosters.values()), str({k: len(v) for k, v in rosters.items()}))

        decline = client.request(
            "POST",
            f"/api/leagues/{league_id}/trades",
            token=commissioner.token,
            body={
                "offerPlayer": rosters[commissioner.email][0],
                "requestPlayer": rosters[manager_a.email][0],
                "targetManager": manager_a.email,
                "note": "release gate decline",
            },
            expected=(201,),
        ).payload
        declined = client.request(
            "POST",
            f"/api/leagues/{league_id}/trades/{decline['id']}/status",
            token=manager_a.token,
            body={"status": "Declined"},
        ).payload
        add_check(checks, "Trade decline lifecycle", declined.get("status") == "Declined", str(declined))

        accepted_offer = client.request(
            "POST",
            f"/api/leagues/{league_id}/trades",
            token=commissioner.token,
            body={
                "offerPlayer": rosters[commissioner.email][0],
                "requestPlayer": rosters[manager_a.email][0],
                "targetManager": manager_a.email,
                "note": "release gate acceptance",
            },
            expected=(201,),
        ).payload
        accepted = client.request(
            "POST",
            f"/api/leagues/{league_id}/trades/{accepted_offer['id']}/status",
            token=manager_a.token,
            body={"status": "Accepted"},
        ).payload
        commissioner_after = client.request("GET", f"/api/leagues/{league_id}/roster", token=commissioner.token).payload
        manager_a_after = client.request("GET", f"/api/leagues/{league_id}/roster", token=manager_a.token).payload
        add_check(
            checks,
            "Accepted trade swaps both rosters",
            accepted.get("status") in {"Accepted", "Approved"}
            and str(rosters[manager_a.email][0]["id"]) in player_ids(commissioner_after)
            and str(rosters[commissioner.email][0]["id"]) in player_ids(manager_a_after),
            f"status={accepted.get('status')}",
        )

        old_b_id = str(rosters[manager_b.email][0]["id"])
        cancelled = client.request(
            "POST",
            f"/api/leagues/{league_id}/waivers",
            token=manager_b.token,
            body={"addPlayer": unique_players[3], "dropPlayerId": old_b_id},
            expected=(201,),
        ).payload
        cancelled_list = client.request(
            "POST",
            f"/api/leagues/{league_id}/waivers/{cancelled['id']}/status",
            token=manager_b.token,
            body={"status": "Cancelled"},
        ).payload
        add_check(
            checks,
            "Waiver cancellation lifecycle",
            any(item.get("id") == cancelled["id"] and item.get("status") == "Cancelled" for item in cancelled_list),
            str(cancelled["id"]),
        )

        claim = client.request(
            "POST",
            f"/api/leagues/{league_id}/waivers",
            token=manager_b.token,
            body={"addPlayer": unique_players[4], "dropPlayerId": old_b_id},
            expected=(201,),
        ).payload
        processed = client.request("POST", f"/api/leagues/{league_id}/waivers/process", token=commissioner.token).payload
        manager_b_after = client.request("GET", f"/api/leagues/{league_id}/roster", token=manager_b.token).payload
        add_check(
            checks,
            "Commissioner waiver processing updates roster",
            str(claim["id"]) in json.dumps(processed)
            and str(unique_players[4]["id"]) in player_ids(manager_b_after)
            and old_b_id not in player_ids(manager_b_after),
            str(processed),
        )

        editable = client.request("GET", f"/api/leagues/{league_id}", token=commissioner.token).payload
        editable["waiverRules"] = {"mode": "free_agency", "claimDeadline": "", "freeAgencyLocked": False}
        client.request("PUT", f"/api/leagues/{league_id}", token=commissioner.token, body=editable)
        client.request(
            "POST",
            f"/api/leagues/{league_id}/roster/drop",
            token=manager_b.token,
            body={"playerId": unique_players[4]["id"]},
        )
        free_roster = client.request(
            "POST",
            f"/api/leagues/{league_id}/roster",
            token=manager_b.token,
            body={"player": unique_players[5]},
        ).payload
        add_check(checks, "Free-agent add/drop lifecycle", str(unique_players[5]["id"]) in player_ids(free_roster), str(free_roster))

        schedule = client.request(
            "POST",
            f"/api/leagues/{league_id}/matchups/generate-season",
            token=commissioner.token,
            body={"weeks": 1},
        ).payload
        add_check(checks, "Season schedule generated", isinstance(schedule, list) and len(schedule) > 0, str(schedule))
        scored = client.request(
            "POST",
            f"/api/leagues/{league_id}/score/week/1",
            token=commissioner.token,
            body={"season": datetime.now(timezone.utc).year},
        ).payload
        add_check(checks, "Scoring calculation completed", scored.get("week") == 1 and isinstance(scored.get("matchups"), list), str(scored))
        finalized = client.request("POST", f"/api/leagues/{league_id}/score/week/1/finalize", token=commissioner.token).payload
        all_final = isinstance(finalized, list) and bool(finalized) and all(str(item.get("status", "")).lower() == "final" for item in finalized)
        add_check(checks, "Scoring week finalized", all_final, str(finalized))
        finalized_again = client.request("POST", f"/api/leagues/{league_id}/score/week/1/finalize", token=commissioner.token).payload
        add_check(
            checks,
            "Week finalization is idempotent",
            isinstance(finalized_again, list) and all(str(item.get("status", "")).lower() == "final" for item in finalized_again),
            str(finalized_again),
        )

        standings = derive_standings(finalized, [account.email for account in accounts])
        add_check(checks, "Final matchups produce standings evidence", {row["managerEmail"] for row in standings} == {a.email for a in accounts}, str(standings))
        evidence["derivedStandings"] = standings
        evidence["finalMatchups"] = finalized

        transactions = client.request("GET", f"/api/leagues/{league_id}/transactions", token=commissioner.token).payload
        transaction_text = json.dumps(transactions).lower()
        required_events = ("draft pick", "trade", "waiver processed", "scoring finalized")
        add_check(checks, "Lifecycle transaction audit trail", all(event in transaction_text for event in required_events), f"events={required_events}")
        evidence["transactionCount"] = len(transactions) if isinstance(transactions, list) else 0

    except Exception as exc:
        failure = exc
    finally:
        if not keep_resources:
            for owner, resource_id, label in (
                (commissioner, league_id, "primaryLeague"),
                (manager_b, isolation_id, "isolationLeague"),
            ):
                if resource_id and owner.token:
                    try:
                        client.request("DELETE", f"/api/leagues/{resource_id}", token=owner.token, expected=(200, 404))
                        evidence[f"{label}Cleaned"] = True
                    except Exception as cleanup_error:
                        evidence[f"{label}CleanupError"] = str(cleanup_error)

    add_check(
        checks,
        "Lifecycle execution completed",
        failure is None,
        "all lifecycle stages completed" if failure is None else str(failure),
    )
    passed = all(check["passed"] or not check["required"] for check in checks)
    report = {
        "title": "Full fantasy lifecycle validation",
        "generatedAt": utc_now(),
        "status": "passed" if passed else "failed",
        "classification": "lifecycle-ready" if passed else "lifecycle-blocked",
        "apiBaseUrl": base_url,
        "checks": checks,
        "evidence": evidence,
        "summary": "The staging gate exercises login, league isolation, membership approval, commissioner authorization, draft order and queues, complete rosters, trades, waivers, free agency, schedule generation, scoring, repeatable finalization, standings evidence, transactions, and cleanup.",
    }
    write_report(report, report_dir, "full-lifecycle-validation")
    enforce_checks(checks)
    print("Full lifecycle validation passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except GateFailure as exc:
        write_failure_report("Full fantasy lifecycle validation", "full-lifecycle-validation", exc)
        print(f"Full lifecycle validation failed: {exc}", file=os.sys.stderr)
        raise SystemExit(1)
