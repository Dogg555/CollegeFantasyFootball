#!/usr/bin/env python3
"""Simulate a complete multi-account fantasy league lifecycle.

The simulator is designed for the disposable environment in docker-compose.sim.yml.
It uses only the Python standard library and never needs production credentials.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import random
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable


DEFAULT_BASE_URL = "http://127.0.0.1:18080"
DEFAULT_PASSWORD = "LocalSimulation123!"


class SimulationFailure(RuntimeError):
    """Raised when a required simulation assertion fails."""


class ApiError(RuntimeError):
    def __init__(self, method: str, path: str, status: int, payload: Any):
        self.method = method
        self.path = path
        self.status = status
        self.payload = payload
        message = payload.get("error") if isinstance(payload, dict) else str(payload)
        super().__init__(f"{method} {path} returned HTTP {status}: {message}")


@dataclass
class Account:
    email: str
    password: str = DEFAULT_PASSWORD
    token: str = ""
    team_name: str = ""


@dataclass
class Check:
    name: str
    passed: bool
    detail: str = ""


@dataclass
class IterationResult:
    iteration: int
    league_id: str = ""
    duration_seconds: float = 0.0
    checks: list[Check] = field(default_factory=list)
    error: str = ""

    @property
    def passed(self) -> bool:
        return not self.error and all(check.passed for check in self.checks)


class ApiClient:
    def __init__(self, base_url: str, timeout: float = 45.0):
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout

    def request_result(
        self,
        method: str,
        path: str,
        *,
        token: str = "",
        body: Any | None = None,
    ) -> tuple[int, Any]:
        payload = None
        headers = {"Accept": "application/json"}
        if body is not None:
            payload = json.dumps(body).encode("utf-8")
            headers["Content-Type"] = "application/json"
        if token:
            headers["Authorization"] = f"Bearer {token}"
        request = urllib.request.Request(
            f"{self.base_url}{path}",
            data=payload,
            headers=headers,
            method=method,
        )
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response:
                status = response.getcode()
                text = response.read().decode("utf-8")
        except urllib.error.HTTPError as exc:
            status = exc.code
            text = exc.read().decode("utf-8")
        except urllib.error.URLError as exc:
            raise SimulationFailure(
                f"Could not reach {self.base_url} while calling {method} {path}: {exc}"
            ) from exc
        try:
            parsed = json.loads(text) if text else {}
        except json.JSONDecodeError:
            parsed = {"raw": text}
        return status, parsed

    def request(
        self,
        method: str,
        path: str,
        *,
        token: str = "",
        body: Any | None = None,
        expected: Iterable[int] = (200,),
    ) -> Any:
        status, payload = self.request_result(method, path, token=token, body=body)
        expected_set = set(expected)
        if status not in expected_set:
            raise ApiError(method, path, status, payload)
        return payload


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SimulationFailure(message)


def encoded(value: str) -> str:
    return urllib.parse.quote(value, safe="")


def player_ids(roster: Any) -> set[str]:
    if not isinstance(roster, list):
        return set()
    return {
        str(player.get("id"))
        for player in roster
        if isinstance(player, dict) and player.get("id")
    }


def check(result: IterationResult, name: str, passed: bool, detail: str = "") -> None:
    result.checks.append(Check(name=name, passed=passed, detail=detail))
    require(passed, f"{name}: {detail or 'failed'}")


def signup(client: ApiClient, account: Account) -> None:
    payload = client.request(
        "POST",
        "/api/auth/signup",
        body={"email": account.email, "password": account.password},
        expected=(201,),
    )
    account.token = str(payload.get("token", ""))
    require(bool(account.token), f"Signup did not return a token for {account.email}")


def login(client: ApiClient, account: Account) -> None:
    payload = client.request(
        "POST",
        "/api/auth/login",
        body={"email": account.email, "password": account.password},
    )
    account.token = str(payload.get("token", ""))
    require(bool(account.token), f"Login did not return a token for {account.email}")


def load_players(client: ApiClient, minimum: int, rng: random.Random) -> list[dict[str, Any]]:
    catalog = client.request("GET", "/api/players?limit=100&offset=0")
    players = [
        player
        for player in catalog if isinstance(player, dict) and str(player.get("id", "")).startswith("sim-player-")
    ] if isinstance(catalog, list) else []
    require(
        len(players) >= minimum,
        f"Simulation requires at least {minimum} seeded sim-player records; found {len(players)}. "
        "Run the local simulation launcher so scripts/sim_seed.sql is applied.",
    )
    rng.shuffle(players)
    return players


def unique_accounts(iteration: int, team_count: int, run_id: str) -> list[Account]:
    accounts: list[Account] = []
    for index in range(team_count):
        role = "commissioner" if index == 0 else f"manager{index}"
        accounts.append(
            Account(
                email=f"sim-{run_id}-{iteration}-{role}@example.test",
                team_name=f"Sim Team {iteration}-{index + 1}",
            )
        )
    return accounts


def create_league(client: ApiClient, accounts: list[Account], iteration: int) -> dict[str, Any]:
    commissioner = accounts[0]
    return client.request(
        "POST",
        "/api/leagues",
        token=commissioner.token,
        expected=(201,),
        body={
            "name": f"Local Simulation League {iteration}",
            "teams": len(accounts),
            "scoring": "ppr",
            "draftType": "snake",
            "draftLobbyOpen": True,
            "rosterRules": {"qb": 0, "rb": 0, "wr": 0, "te": 0, "flex": 0, "bench": 1},
            "waiverRules": {"mode": "waivers", "claimDeadline": "", "freeAgencyLocked": True},
            "tradeRules": {"commissionerApproval": False, "expirationHours": 48},
            "invitedEmails": [account.email for account in accounts[1:]],
            "notes": "local-simulation-only",
        },
    )


def join_and_name_accounts(
    client: ApiClient,
    result: IterationResult,
    league_id: str,
    accounts: list[Account],
) -> None:
    commissioner = accounts[0]
    join_info = client.request(
        "GET",
        f"/api/leagues/{league_id}/join-info",
        token=commissioner.token,
    )
    join_code = str(join_info.get("joinCode", ""))
    check(result, "Join code created", len(join_code.replace("-", "")) == 8, join_code)

    client.request(
        "PUT",
        f"/api/leagues/{league_id}/team-name",
        token=commissioner.token,
        body={"teamName": commissioner.team_name},
    )

    for account in accounts[1:]:
        joined = client.request(
            "POST",
            "/api/leagues/join",
            token=account.token,
            body={"code": join_code.lower()},
            expected=(200, 202),
        )
        client.request(
            "PUT",
            f"/api/leagues/{league_id}/team-name",
            token=account.token,
            body={"teamName": account.team_name},
        )
        if joined.get("joinStatus") == "pending_approval":
            client.request(
                "PUT",
                f"/api/leagues/{league_id}/members/{encoded(account.email)}",
                token=commissioner.token,
                body={"role": "member", "status": "Active", "teamName": account.team_name},
            )

    members = client.request(
        "GET",
        f"/api/leagues/{league_id}/members",
        token=commissioner.token,
    )
    active = {
        str(member.get("email", "")).lower()
        for member in members
        if isinstance(member, dict) and str(member.get("status", "")).lower() == "active"
    }
    names = {
        str(member.get("teamName", "")).strip().lower()
        for member in members
        if isinstance(member, dict) and str(member.get("status", "")).lower() == "active"
    }
    check(
        result,
        "All managers joined and active",
        {account.email for account in accounts}.issubset(active),
        f"active={len(active)} expected={len(accounts)}",
    )
    check(
        result,
        "Team names unique and persisted",
        len(names) == len(accounts) and "" not in names,
        f"names={sorted(names)}",
    )


def verify_authorization(
    client: ApiClient,
    result: IterationResult,
    league_id: str,
    accounts: list[Account],
) -> None:
    current = client.request("GET", f"/api/leagues/{league_id}", token=accounts[0].token)
    status, payload = client.request_result(
        "PUT",
        f"/api/leagues/{league_id}/settings",
        token=accounts[1].token,
        body=current,
    )
    check(
        result,
        "Manager cannot edit commissioner settings",
        status == 403,
        f"status={status} payload={payload}",
    )


def run_draft(
    client: ApiClient,
    result: IterationResult,
    league_id: str,
    accounts: list[Account],
    players: list[dict[str, Any]],
    race_enabled: bool,
) -> dict[str, list[dict[str, Any]]]:
    commissioner = accounts[0]
    order = [account.email for account in accounts]
    saved_order = client.request(
        "PUT",
        f"/api/leagues/{league_id}/draft/order",
        token=commissioner.token,
        body={"draftOrder": order},
    )
    check(result, "Draft order saved", saved_order.get("draftOrder") == order, str(saved_order))

    queue = client.request(
        "PUT",
        f"/api/leagues/{league_id}/draft/queue",
        token=accounts[1].token,
        body={"queue": [players[-1], players[-2]]},
    )
    check(result, "Manager draft queue isolated", len(queue.get("queue", [])) == 2, str(queue))

    next_player_index = 0
    if race_enabled:
        def pick_once() -> tuple[int, Any]:
            return client.request_result(
                "POST",
                f"/api/leagues/{league_id}/draft/picks",
                token=commissioner.token,
                body={"player": players[0]},
            )

        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
            outcomes = list(executor.map(lambda _: pick_once(), range(2)))
        success_count = sum(1 for status, _ in outcomes if status == 201)
        rejected_count = sum(1 for status, _ in outcomes if status in {400, 403, 409})
        check(
            result,
            "Concurrent duplicate pick protection",
            success_count == 1 and rejected_count == 1,
            f"outcomes={[status for status, _ in outcomes]}",
        )
        next_player_index = 1
    else:
        client.request(
            "POST",
            f"/api/leagues/{league_id}/draft/picks",
            token=commissioner.token,
            body={"player": players[0]},
            expected=(201,),
        )
        next_player_index = 1

    state: dict[str, Any] = {}
    for account, player in zip(accounts[1:], players[next_player_index:len(accounts)]):
        state = client.request(
            "POST",
            f"/api/leagues/{league_id}/draft/picks",
            token=account.token,
            body={"player": player},
            expected=(201,),
        )

    check(
        result,
        "Draft completed",
        state.get("status") == "complete" and len(state.get("picks", [])) == len(accounts),
        f"status={state.get('status')} picks={len(state.get('picks', []))}",
    )

    rosters = {
        account.email: client.request(
            "GET",
            f"/api/leagues/{league_id}/roster",
            token=account.token,
        )
        for account in accounts
    }
    check(
        result,
        "Drafted rosters persisted",
        all(isinstance(roster, list) and len(roster) == 1 for roster in rosters.values()),
        str({email: len(roster) for email, roster in rosters.items()}),
    )
    all_ids = [player_id for roster in rosters.values() for player_id in player_ids(roster)]
    check(
        result,
        "No player drafted twice",
        len(all_ids) == len(set(all_ids)) == len(accounts),
        str(all_ids),
    )
    return rosters


def run_trade(
    client: ApiClient,
    result: IterationResult,
    league_id: str,
    accounts: list[Account],
    rosters: dict[str, list[dict[str, Any]]],
) -> None:
    commissioner, manager = accounts[0], accounts[1]
    offer = client.request(
        "POST",
        f"/api/leagues/{league_id}/trades",
        token=commissioner.token,
        body={
            "offerPlayer": rosters[commissioner.email][0],
            "requestPlayer": rosters[manager.email][0],
            "targetManager": manager.email,
            "note": "local simulation trade",
        },
        expected=(201,),
    )
    accepted = client.request(
        "POST",
        f"/api/leagues/{league_id}/trades/{offer['id']}/status",
        token=manager.token,
        body={"status": "Accepted"},
    )
    commissioner_after = client.request(
        "GET", f"/api/leagues/{league_id}/roster", token=commissioner.token
    )
    manager_after = client.request(
        "GET", f"/api/leagues/{league_id}/roster", token=manager.token
    )
    check(
        result,
        "Accepted trade swapped both rosters",
        accepted.get("status") in {"Accepted", "Approved"}
        and str(rosters[manager.email][0]["id"]) in player_ids(commissioner_after)
        and str(rosters[commissioner.email][0]["id"]) in player_ids(manager_after),
        f"status={accepted.get('status')}",
    )


def run_waiver_and_free_agency(
    client: ApiClient,
    result: IterationResult,
    league_id: str,
    accounts: list[Account],
    players: list[dict[str, Any]],
) -> None:
    commissioner = accounts[0]
    waiver_manager = accounts[2]
    roster_before = client.request(
        "GET", f"/api/leagues/{league_id}/roster", token=waiver_manager.token
    )
    old_id = str(roster_before[0]["id"])
    waiver_player = players[len(accounts) + 1]
    claim = client.request(
        "POST",
        f"/api/leagues/{league_id}/waivers",
        token=waiver_manager.token,
        body={"addPlayer": waiver_player, "dropPlayerId": old_id},
        expected=(201,),
    )
    processed = client.request(
        "POST",
        f"/api/leagues/{league_id}/waivers/process",
        token=commissioner.token,
    )
    roster_after = client.request(
        "GET", f"/api/leagues/{league_id}/roster", token=waiver_manager.token
    )
    check(
        result,
        "Waiver processing updated roster",
        str(claim.get("id", "")) in json.dumps(processed)
        and str(waiver_player["id"]) in player_ids(roster_after)
        and old_id not in player_ids(roster_after),
        str(processed),
    )

    free_agent_manager = accounts[-1]
    settings = client.request("GET", f"/api/leagues/{league_id}", token=commissioner.token)
    settings["waiverRules"] = {
        "mode": "free_agency",
        "claimDeadline": "",
        "freeAgencyLocked": False,
    }
    client.request(
        "PUT",
        f"/api/leagues/{league_id}/settings",
        token=commissioner.token,
        body=settings,
    )
    free_before = client.request(
        "GET", f"/api/leagues/{league_id}/roster", token=free_agent_manager.token
    )
    drop_id = str(free_before[0]["id"])
    client.request(
        "POST",
        f"/api/leagues/{league_id}/roster/drop",
        token=free_agent_manager.token,
        body={"playerId": drop_id},
    )
    replacement = players[len(accounts) + 2]
    free_after = client.request(
        "POST",
        f"/api/leagues/{league_id}/roster",
        token=free_agent_manager.token,
        body={"player": replacement},
    )
    check(
        result,
        "Free-agent add/drop updated roster",
        str(replacement["id"]) in player_ids(free_after) and drop_id not in player_ids(free_after),
        str(free_after),
    )


def run_scoring(
    client: ApiClient,
    result: IterationResult,
    league_id: str,
    commissioner: Account,
) -> None:
    schedule = client.request(
        "POST",
        f"/api/leagues/{league_id}/matchups/generate-season",
        token=commissioner.token,
        body={"weeks": 1},
    )
    check(result, "Season schedule generated", isinstance(schedule, list) and bool(schedule), str(schedule))
    scored = client.request(
        "POST",
        f"/api/leagues/{league_id}/score/week/1",
        token=commissioner.token,
        body={"season": datetime.now(timezone.utc).year},
    )
    check(
        result,
        "Week scoring completed",
        scored.get("week") == 1 and isinstance(scored.get("matchups"), list),
        str(scored),
    )
    finalized = client.request(
        "POST",
        f"/api/leagues/{league_id}/score/week/1/finalize",
        token=commissioner.token,
    )
    all_final = isinstance(finalized, list) and bool(finalized) and all(
        str(matchup.get("status", "")).lower() == "final" for matchup in finalized
    )
    check(result, "Week finalized", all_final, str(finalized))
    finalized_again = client.request(
        "POST",
        f"/api/leagues/{league_id}/score/week/1/finalize",
        token=commissioner.token,
    )
    check(
        result,
        "Week finalization idempotent",
        isinstance(finalized_again, list)
        and all(str(matchup.get("status", "")).lower() == "final" for matchup in finalized_again),
        str(finalized_again),
    )


def verify_persistence(
    client: ApiClient,
    result: IterationResult,
    league_id: str,
    accounts: list[Account],
) -> None:
    for account in accounts:
        login(client, account)
        leagues = client.request("GET", "/api/leagues", token=account.token)
        require(
            any(isinstance(league, dict) and league.get("id") == league_id for league in leagues),
            f"{account.email} could not reload league {league_id}",
        )
        roster = client.request("GET", f"/api/leagues/{league_id}/roster", token=account.token)
        require(isinstance(roster, list) and len(roster) == 1, f"Roster did not persist for {account.email}")
    check(result, "Login and reload persistence", True, f"accounts={len(accounts)}")


def verify_audit_trail(
    client: ApiClient,
    result: IterationResult,
    league_id: str,
    commissioner: Account,
) -> None:
    transactions = client.request(
        "GET",
        f"/api/leagues/{league_id}/transactions",
        token=commissioner.token,
    )
    text = json.dumps(transactions).lower()
    required = ("draft pick", "trade", "waiver processed", "scoring finalized")
    check(
        result,
        "Transaction audit trail complete",
        all(event in text for event in required),
        f"required={required} count={len(transactions) if isinstance(transactions, list) else 0}",
    )


def run_iteration(
    client: ApiClient,
    iteration: int,
    team_count: int,
    seed: int,
    run_id: str,
    keep_data: bool,
    race_enabled: bool,
) -> IterationResult:
    started = time.monotonic()
    result = IterationResult(iteration=iteration)
    accounts = unique_accounts(iteration, team_count, run_id)
    league_id = ""
    try:
        health = client.request("GET", "/health")
        check(
            result,
            "API and database healthy",
            health.get("status") == "ok" and health.get("database") == "ok",
            str(health),
        )
        for account in accounts:
            signup(client, account)
        check(result, "Simulation accounts created", True, f"accounts={len(accounts)}")

        rng = random.Random(seed + iteration)
        players = load_players(client, team_count + 8, rng)
        check(result, "Seeded player catalog available", True, f"players={len(players)}")

        league = create_league(client, accounts, iteration)
        league_id = str(league.get("id", ""))
        result.league_id = league_id
        check(result, "League created", bool(league_id), league_id)

        join_and_name_accounts(client, result, league_id, accounts)
        verify_authorization(client, result, league_id, accounts)
        rosters = run_draft(client, result, league_id, accounts, players, race_enabled)
        run_trade(client, result, league_id, accounts, rosters)
        run_waiver_and_free_agency(client, result, league_id, accounts, players)
        run_scoring(client, result, league_id, accounts[0])
        verify_audit_trail(client, result, league_id, accounts[0])
        verify_persistence(client, result, league_id, accounts)
    except Exception as exc:  # noqa: BLE001 - report the entire simulation failure
        result.error = str(exc)
    finally:
        if league_id and not keep_data and accounts[0].token:
            status, payload = client.request_result(
                "DELETE",
                f"/api/leagues/{league_id}",
                token=accounts[0].token,
            )
            if status not in {200, 404} and not result.error:
                result.error = f"Cleanup failed with HTTP {status}: {payload}"
        result.duration_seconds = round(time.monotonic() - started, 3)
    return result


def result_payload(
    base_url: str,
    team_count: int,
    seed: int,
    results: list[IterationResult],
) -> dict[str, Any]:
    passed = all(result.passed for result in results)
    return {
        "status": "passed" if passed else "failed",
        "generatedAt": datetime.now(timezone.utc).isoformat(),
        "baseUrl": base_url,
        "teams": team_count,
        "seed": seed,
        "iterations": len(results),
        "passedIterations": sum(1 for result in results if result.passed),
        "durationSeconds": round(sum(result.duration_seconds for result in results), 3),
        "results": [
            {
                "iteration": result.iteration,
                "status": "passed" if result.passed else "failed",
                "leagueId": result.league_id,
                "durationSeconds": result.duration_seconds,
                "error": result.error,
                "checks": [
                    {"name": item.name, "passed": item.passed, "detail": item.detail}
                    for item in result.checks
                ],
            }
            for result in results
        ],
    }


def print_summary(payload: dict[str, Any]) -> None:
    print("\nCollege Fantasy Football League Simulation")
    print("=" * 44)
    print(f"Status:      {payload['status'].upper()}")
    print(f"Teams:       {payload['teams']}")
    print(f"Iterations:  {payload['passedIterations']}/{payload['iterations']} passed")
    print(f"Duration:    {payload['durationSeconds']:.3f}s")
    for item in payload["results"]:
        marker = "PASS" if item["status"] == "passed" else "FAIL"
        print(
            f"[{marker}] iteration {item['iteration']} "
            f"({item['durationSeconds']:.3f}s) league={item['leagueId'] or '--'}"
        )
        if item["error"]:
            print(f"       {item['error']}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Simulate a complete 4- or 6-team league lifecycle against a disposable API.",
    )
    parser.add_argument(
        "--base-url",
        default=os.environ.get("CFF_API_BASE_URL", DEFAULT_BASE_URL),
        help=f"API origin without /api (default: {DEFAULT_BASE_URL})",
    )
    parser.add_argument("--teams", type=int, choices=(4, 6), default=4)
    parser.add_argument("--iterations", type=int, default=1)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--timeout", type=float, default=45.0)
    parser.add_argument("--keep-data", action="store_true")
    parser.add_argument("--skip-concurrency", action="store_true")
    parser.add_argument("--report-dir", default="simulation-artifacts")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    require(args.iterations >= 1, "--iterations must be at least 1")
    require(args.iterations <= 100, "--iterations cannot exceed 100")

    client = ApiClient(args.base_url, timeout=args.timeout)
    run_id = f"{int(time.time())}-{os.getpid()}"
    results: list[IterationResult] = []
    for iteration in range(1, args.iterations + 1):
        result = run_iteration(
            client,
            iteration,
            args.teams,
            args.seed,
            run_id,
            args.keep_data,
            not args.skip_concurrency,
        )
        results.append(result)
        if not result.passed:
            break

    payload = result_payload(args.base_url, args.teams, args.seed, results)
    report_dir = Path(args.report_dir)
    report_dir.mkdir(parents=True, exist_ok=True)
    report_path = report_dir / "league-simulation-report.json"
    report_path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    print_summary(payload)
    print(f"Report:      {report_path}")
    return 0 if payload["status"] == "passed" else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (SimulationFailure, ApiError) as exc:
        print(f"League simulation failed: {exc}", file=sys.stderr)
        raise SystemExit(1)
