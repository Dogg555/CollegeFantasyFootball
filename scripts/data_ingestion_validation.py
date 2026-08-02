#!/usr/bin/env python3
"""Validate current-season player, schedule, and live-score ingestion quality."""
from __future__ import annotations

import argparse
import os
import time
import urllib.parse
from collections import Counter
from datetime import datetime, timezone
from typing import Any

from release_gate_common import (
    GateFailure,
    JsonHttpClient,
    add_check,
    enforce_checks,
    env_flag,
    int_env,
    require_env,
    utc_now,
    write_report,
    write_failure_report,
)


REQUIRED_PLAYER_FIELDS = ("id", "name", "team", "position", "conference")


def evaluate_player_page(players: list[dict[str, Any]], expected_season: int) -> dict[str, Any]:
    missing = Counter()
    ids: list[str] = []
    seasons = Counter()
    teams = Counter()
    positions = Counter()
    conferences = Counter()
    for player in players:
        for field in REQUIRED_PLAYER_FIELDS:
            if not str(player.get(field, "")).strip():
                missing[field] += 1
        player_id = str(player.get("id", "")).strip()
        if player_id:
            ids.append(player_id)
        try:
            season = int(player.get("season", 0) or 0)
        except (TypeError, ValueError):
            season = 0
        seasons[season] += 1
        teams[str(player.get("team", "")).strip()] += 1
        positions[str(player.get("position", "")).strip().upper()] += 1
        conferences[str(player.get("conference", "")).strip()] += 1
    return {
        "count": len(players),
        "ids": ids,
        "duplicateIds": len(ids) - len(set(ids)),
        "missing": dict(missing),
        "seasons": dict(seasons),
        "expectedSeasonRows": seasons.get(expected_season, 0),
        "teams": dict(teams),
        "positions": dict(positions),
        "conferences": dict(conferences),
    }


def query(client: JsonHttpClient, path: str, params: dict[str, Any]) -> Any:
    encoded = urllib.parse.urlencode({key: value for key, value in params.items() if value is not None})
    return client.request("GET", f"{path}?{encoded}").payload


def contains_casefold(value: Any, expected: str) -> bool:
    return expected.casefold() in str(value or "").casefold()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-player-ingest", action="store_true", help="Trigger the full player ingestion before validation.")
    parser.add_argument("--run-live-ingest", action="store_true", help="Trigger live/schedule ingestion before validation.")
    args = parser.parse_args()

    base_url = require_env("CFF_API_BASE_URL")
    admin_token = require_env("CFF_ADMIN_API_TOKEN")
    expected_season = int_env("CFF_DATA_EXPECTED_SEASON", datetime.now(timezone.utc).year, 2000, 2100)
    min_players = int_env("CFF_DATA_MIN_ACTIVE_PLAYERS", 1000, 1)
    min_teams = int_env("CFF_DATA_MIN_TEAMS", 100, 1)
    min_conferences = int_env("CFF_DATA_MIN_CONFERENCES", 8, 1)
    sample_pages = int_env("CFF_DATA_SAMPLE_PAGES", 5, 1, 25)
    page_size = int_env("CFF_DATA_PAGE_SIZE", 100, 1, 100)
    max_missing_percent = int_env("CFF_DATA_MAX_MISSING_PERCENT", 2, 0, 100)
    max_monthly_calls = int_env("CFF_DATA_MAX_MONTHLY_CFBD_CALLS", 125000, 1)
    require_schedule = env_flag("CFF_DATA_REQUIRE_SCHEDULE", True)
    report_dir = os.environ.get("CFF_RELEASE_GATE_REPORT_DIR", "release-gate-artifacts")
    client = JsonHttpClient(base_url, timeout=120)
    checks: list[dict[str, Any]] = []
    evidence: dict[str, Any] = {}

    health = client.request("GET", "/health").payload
    add_check(checks, "API health", health.get("status") == "ok", str(health))
    add_check(checks, "Database health", health.get("database") == "ok", f"database={health.get('database')}")

    if args.run_player_ingest:
        started = time.monotonic()
        ingest_result = client.request(
            "POST",
            "/api/admin/ingest/cfbd",
            token=admin_token,
            expected=(200,),
        ).payload
        evidence["playerIngest"] = ingest_result
        add_check(checks, "Triggered player ingestion completed", ingest_result.get("status") == "ok", str(ingest_result))
        evidence["playerIngestSeconds"] = round(time.monotonic() - started, 2)

    if args.run_live_ingest:
        started = time.monotonic()
        live_result = client.request(
            "POST",
            "/api/admin/ingest/cfbd/live",
            token=admin_token,
            expected=(200,),
        ).payload
        evidence["liveIngest"] = live_result
        add_check(checks, "Triggered live ingestion completed", live_result.get("status") == "ok", str(live_result))
        evidence["liveIngestSeconds"] = round(time.monotonic() - started, 2)

    ingest_status = client.request("GET", "/api/admin/ingest/cfbd/status", token=admin_token).payload
    evidence["ingestionStatus"] = ingest_status
    add_check(checks, "Ingestion service ready", ingest_status.get("status") == "ok" and ingest_status.get("configured") is True, str(ingest_status))
    add_check(checks, "CFBD credential configured", ingest_status.get("cfbdApiConfigured") is True, f"cfbdApiConfigured={ingest_status.get('cfbdApiConfigured')}")
    add_check(checks, "Full roster schedule is weekly", ingest_status.get("fullRosterSchedule") == "weekly", f"schedule={ingest_status.get('fullRosterSchedule')}")
    add_check(checks, "Manual ingestion remains available", ingest_status.get("manualTriggerAvailable") is True, str(ingest_status.get("manualTriggerAvailable")))

    player_runs = [run for run in ingest_status.get("runs", []) if run.get("resource") == "players"]
    latest_player_run = player_runs[0] if player_runs else {}
    add_check(
        checks,
        "Latest player ingestion succeeded",
        latest_player_run.get("status") == "success" and not latest_player_run.get("error"),
        str(latest_player_run),
    )
    add_check(
        checks,
        "Latest player ingestion uses current season",
        int(latest_player_run.get("season", 0) or 0) == expected_season,
        f"latest={latest_player_run.get('season')}; expected={expected_season}",
    )

    meta = client.request("GET", "/api/players/meta").payload
    evidence["playerMeta"] = meta
    active_players = int(meta.get("activePlayers", 0) or 0)
    team_count = int(meta.get("teams", 0) or 0)
    conference_count = int(meta.get("conferences", 0) or 0)
    add_check(checks, "Player catalog status", meta.get("status") == "ok", str(meta))
    add_check(checks, "Current player population", active_players >= min_players, f"activePlayers={active_players}; minimum={min_players}")
    add_check(checks, "FBS team coverage", team_count >= min_teams, f"teams={team_count}; minimum={min_teams}")
    add_check(checks, "Conference coverage", conference_count >= min_conferences, f"conferences={conference_count}; minimum={min_conferences}")
    add_check(checks, "Catalog season", int(meta.get("season", 0) or 0) == expected_season, f"season={meta.get('season')}; expected={expected_season}")
    position_counts = meta.get("positions") if isinstance(meta.get("positions"), dict) else {}
    for position in ("QB", "RB", "WR", "TE"):
        add_check(checks, f"Position population: {position}", int(position_counts.get(position, 0) or 0) > 0, f"count={position_counts.get(position, 0)}")
    kicker_count = sum(int(position_counts.get(alias, 0) or 0) for alias in ("K", "PK", "P/K"))
    add_check(checks, "Position population: K", kicker_count > 0, f"K/PK/P-K count={kicker_count}")

    sampled: list[dict[str, Any]] = []
    for page in range(sample_pages):
        rows = query(client, "/api/players", {"limit": page_size, "offset": page * page_size})
        if not isinstance(rows, list):
            raise GateFailure(f"Player page {page} was not a JSON array")
        sampled.extend(item for item in rows if isinstance(item, dict))
        if len(rows) < page_size:
            break
    sample_evaluation = evaluate_player_page(sampled, expected_season)
    evidence["playerSample"] = {
        key: value for key, value in sample_evaluation.items() if key not in {"ids", "teams", "positions", "conferences"}
    }
    total_sampled = max(1, sample_evaluation["count"])
    missing_total = sum(sample_evaluation["missing"].values())
    missing_percent = (missing_total * 100.0) / (total_sampled * len(REQUIRED_PLAYER_FIELDS))
    add_check(checks, "Player pages returned rows", sample_evaluation["count"] > 0, f"sampled={sample_evaluation['count']}")
    add_check(checks, "Player pagination has no duplicate IDs", sample_evaluation["duplicateIds"] == 0, f"duplicates={sample_evaluation['duplicateIds']}")
    add_check(checks, "Player required fields are populated", missing_percent <= max_missing_percent, f"missingPercent={missing_percent:.2f}; maximum={max_missing_percent}")
    add_check(
        checks,
        "Sampled players use current season",
        sample_evaluation["expectedSeasonRows"] == sample_evaluation["count"],
        f"currentSeasonRows={sample_evaluation['expectedSeasonRows']}; sampled={sample_evaluation['count']}; seasons={sample_evaluation['seasons']}",
    )

    if sampled:
        sample = sampled[0]
        name_token = str(sample.get("name", "")).split()[0]
        if name_token:
            name_rows = query(client, "/api/players", {"query": name_token, "limit": 25})
            add_check(checks, "Player search by name", isinstance(name_rows, list) and any(contains_casefold(row.get("name"), name_token) for row in name_rows), f"query={name_token}; results={len(name_rows) if isinstance(name_rows, list) else 'invalid'}")
        for field, parameter in (("team", "team"), ("position", "position"), ("conference", "conference")):
            value = str(sample.get(field, "")).strip()
            if not value:
                continue
            rows = query(client, "/api/players", {parameter: value, "limit": 100})
            valid = isinstance(rows, list) and len(rows) > 0 and all(contains_casefold(row.get(field), value) for row in rows)
            add_check(checks, f"Player filter by {field}", valid, f"filter={value}; results={len(rows) if isinstance(rows, list) else 'invalid'}")

    schedule_meta = client.request("GET", "/api/scores/live/meta").payload
    games = client.request("GET", "/api/scores/live").payload
    if not isinstance(games, list):
        games = []
    evidence["scheduleMeta"] = schedule_meta
    evidence["scheduleSample"] = games[:10]
    game_ids = [str(game.get("id", "")) for game in games if isinstance(game, dict) and game.get("id")]
    schedule_count = int(schedule_meta.get("scheduleGameCount", schedule_meta.get("gameCount", 0)) or 0)
    add_check(checks, "Schedule cache endpoint", schedule_meta.get("status") in {"ok", "never", "failed"}, str(schedule_meta))
    add_check(checks, "Schedule cache populated", schedule_count > 0 and len(games) > 0, f"metaCount={schedule_count}; returned={len(games)}", required=require_schedule)
    add_check(checks, "Schedule IDs are unique", len(game_ids) == len(set(game_ids)), f"games={len(game_ids)}; unique={len(set(game_ids))}", required=require_schedule)
    if games:
        current_games = [game for game in games if int(game.get("season", 0) or 0) == expected_season]
        valid_shape = all(
            str(game.get("id", "")).strip()
            and int(game.get("week", 0) or 0) >= 0
            and str(game.get("home", "")).strip()
            and str(game.get("away", "")).strip()
            and str(game.get("startDate", "")).strip()
            and str(game.get("status", "")).strip()
            for game in games
            if isinstance(game, dict)
        )
        add_check(checks, "Schedule uses expected season", len(current_games) == len(games), f"current={len(current_games)}; total={len(games)}; expected={expected_season}", required=require_schedule)
        add_check(checks, "Schedule game fields are complete", valid_shape, f"validated={len(games)}", required=require_schedule)

    live_status = client.request("GET", "/api/admin/ingest/cfbd/live/status", token=admin_token).payload
    evidence["liveIngestionStatus"] = live_status
    monthly_calls = int(live_status.get("monthlyApiCalls", 0) or 0)
    add_check(checks, "Live ingestion configured", live_status.get("configured") is True and live_status.get("databaseConfigured") is True, str(live_status))
    add_check(checks, "Live ingestion has run", live_status.get("status") in {"ok", "idle"}, f"status={live_status.get('status')}", required=require_schedule)
    add_check(checks, "CFBD monthly call budget", monthly_calls < max_monthly_calls, f"calls={monthly_calls}; maximum={max_monthly_calls}")

    passed = all(check["passed"] or not check["required"] for check in checks)
    report = {
        "title": "Data ingestion validation",
        "generatedAt": utc_now(),
        "status": "passed" if passed else "failed",
        "classification": "data-ready" if passed else "data-blocked",
        "apiBaseUrl": base_url,
        "expectedSeason": expected_season,
        "checks": checks,
        "evidence": evidence,
        "summary": "The gate validates the latest ingestion ledger, active current-season population, FBS coverage, field completeness, pagination uniqueness, search/filter behavior, schedule shape, live-ingestion health, and CFBD call usage.",
    }
    write_report(report, report_dir, "data-ingestion-validation")
    enforce_checks(checks)
    print("Data ingestion validation passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        write_failure_report("Data ingestion validation", "data-ingestion-validation", exc)
        print(f"Data ingestion validation failed: {exc}", file=os.sys.stderr)
        raise SystemExit(1)
