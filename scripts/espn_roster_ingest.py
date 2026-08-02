#!/usr/bin/env python3
"""Resumable one-time ESPN Division I FBS roster bootstrap."""

from __future__ import annotations

import argparse
import json
import os
import random
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any, Iterator, Mapping, Sequence

from espn_team_directory import fbs_teams_url

ESPN_BASE_URL = (
    "https://site.api.espn.com/apis/site/v2/sports/football/college-football"
)
USER_AGENT = "CollegeFantasyFootball-roster-bootstrap/1.3"
EXPECTED_FBS_TEAM_RANGE = range(120, 171)
SCOPE = "division-i-fbs"
RESOURCE = "players_espn_fbs"
TERMINAL_PROGRESS_STATUSES = ("success", "empty")


@dataclass(frozen=True)
class Team:
    id: str
    school: str
    conference: str
    slug: str = ""


@dataclass(frozen=True)
class Player:
    id: str
    full_name: str
    first_name: str
    last_name: str
    position: str
    team: str
    conference: str
    year: str
    height: str
    weight: int | None
    season: int
    raw: dict[str, Any]


class IngestError(RuntimeError):
    """Raised for a non-recoverable bootstrap failure."""


def _text(value: Any) -> str:
    if value is None:
        return ""
    if isinstance(value, str):
        return value.strip()
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        return str(value)
    return ""


def _first_text(value: Mapping[str, Any], keys: Sequence[str]) -> str:
    for key in keys:
        text = _text(value.get(key))
        if text:
            return text
    return ""


def _retry_delay(headers: Mapping[str, str], attempt: int) -> float:
    retry_after = headers.get("Retry-After") or headers.get("retry-after")
    if retry_after:
        try:
            return min(max(float(retry_after), 0.5), 120.0)
        except ValueError:
            pass
    return min(2 ** (attempt - 1) + random.uniform(0.1, 0.8), 30.0)


def fetch_json(url: str, *, timeout: float, retries: int) -> dict[str, Any]:
    last_error: Exception | None = None
    for attempt in range(1, retries + 1):
        request = urllib.request.Request(
            url,
            headers={"Accept": "application/json", "User-Agent": USER_AGENT},
        )
        try:
            with urllib.request.urlopen(request, timeout=timeout) as response:
                payload = json.load(response)
            if not isinstance(payload, dict):
                raise IngestError(f"ESPN returned a non-object response for {url}")
            return payload
        except urllib.error.HTTPError as error:
            last_error = error
            if error.code not in {408, 425, 429, 500, 502, 503, 504}:
                detail = error.read(500).decode("utf-8", "replace")
                raise IngestError(
                    f"ESPN request failed with HTTP {error.code}: {url}: {detail}"
                ) from error
            if attempt < retries:
                delay = _retry_delay(error.headers, attempt)
                print(
                    f"ESPN HTTP {error.code}; retrying in {delay:.1f}s",
                    file=sys.stderr,
                    flush=True,
                )
                time.sleep(delay)
        except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as error:
            last_error = error
            if attempt < retries:
                delay = _retry_delay({}, attempt)
                print(
                    f"ESPN request error; retrying in {delay:.1f}s: {error}",
                    file=sys.stderr,
                    flush=True,
                )
                time.sleep(delay)
    raise IngestError(
        f"ESPN request failed after {retries} attempts: {url}: {last_error}"
    )


def _team_nodes(
    value: Any,
    conference: str = "FBS",
) -> Iterator[tuple[Mapping[str, Any], str]]:
    """Recursively find team objects in ESPN's grouped directory response."""
    if isinstance(value, list):
        for item in value:
            yield from _team_nodes(item, conference)
        return
    if not isinstance(value, Mapping):
        return

    wrapped_team = value.get("team")
    if isinstance(wrapped_team, Mapping):
        yield wrapped_team, conference
        return

    if _first_text(value, ("id", "uid")) and _first_text(
        value, ("location", "shortDisplayName")
    ):
        yield value, conference
        return

    child_conference = conference
    if any(key in value for key in ("teams", "groups", "children")):
        label = _first_text(
            value,
            ("shortName", "name", "displayName", "abbreviation"),
        )
        if label and label.casefold() not in {
            "fbs",
            "ncaa football",
            "college football",
        }:
            child_conference = label

    for child in value.values():
        yield from _team_nodes(child, child_conference)


def parse_teams(payload: Mapping[str, Any]) -> list[Team]:
    teams: dict[str, Team] = {}
    for team, conference in _team_nodes(payload):
        team_id = _first_text(team, ("id", "uid"))
        school = _first_text(
            team,
            ("location", "shortDisplayName", "displayName", "name"),
        )
        if not team_id or not school:
            continue
        teams[team_id] = Team(
            id=team_id,
            school=school,
            conference=conference or "FBS",
            slug=_first_text(team, ("slug", "abbreviation")),
        )
    return sorted(teams.values(), key=lambda item: item.school.casefold())


def fetch_fbs_teams(
    *, timeout: float, retries: int, allow_unexpected_team_count: bool
) -> tuple[list[Team], int]:
    payload = fetch_json(fbs_teams_url(), timeout=timeout, retries=retries)
    teams = parse_teams(payload)
    if len(teams) not in EXPECTED_FBS_TEAM_RANGE and not allow_unexpected_team_count:
        raise IngestError(
            f"ESPN returned {len(teams)} Division I FBS teams; expected 120-170. "
            "Use --allow-unexpected-team-count only after inspecting the response."
        )
    print(f"Found {len(teams)} ESPN Division I FBS teams.", flush=True)
    return teams, 1


def _roster_items(payload: Mapping[str, Any]) -> Iterator[Mapping[str, Any]]:
    athletes = payload.get("athletes")
    if not isinstance(athletes, list):
        return
    for group in athletes:
        if not isinstance(group, Mapping):
            continue
        items = group.get("items")
        if isinstance(items, list):
            for item in items:
                if isinstance(item, Mapping):
                    nested = item.get("athlete")
                    yield nested if isinstance(nested, Mapping) else item
        elif _first_text(group, ("id", "uid")):
            yield group


def _position(athlete: Mapping[str, Any]) -> str:
    value = athlete.get("position")
    if isinstance(value, Mapping):
        return _first_text(value, ("abbreviation", "name", "displayName"))
    return _text(value)


def _class_year(athlete: Mapping[str, Any]) -> str:
    for key in ("experience", "class", "classYear"):
        value = athlete.get(key)
        if isinstance(value, Mapping):
            text = _first_text(
                value,
                ("abbreviation", "displayValue", "name", "shortName", "years"),
            )
        else:
            text = _text(value)
        if text:
            return text
    return _first_text(athlete, ("year",))


def _height(athlete: Mapping[str, Any]) -> str:
    display = _first_text(athlete, ("displayHeight", "heightDisplay"))
    if display:
        return display
    raw = athlete.get("height")
    try:
        inches = int(round(float(raw)))
    except (TypeError, ValueError):
        return _text(raw)
    return f"{inches // 12}' {inches % 12}\"" if 48 <= inches <= 96 else str(inches)


def _weight(athlete: Mapping[str, Any]) -> int | None:
    raw = athlete.get("weight")
    if isinstance(raw, (int, float)) and not isinstance(raw, bool):
        return int(round(raw))
    text = _first_text(athlete, ("displayWeight", "weightDisplay")) or _text(raw)
    match = re.search(r"\d+", text)
    return int(match.group(0)) if match else None


def parse_player(
    athlete: Mapping[str, Any], team: Team, season: int
) -> Player | None:
    athlete_id = _first_text(athlete, ("id", "uid"))
    if not athlete_id:
        return None
    first_name = _first_text(athlete, ("firstName", "first_name"))
    last_name = _first_text(athlete, ("lastName", "last_name"))
    full_name = _first_text(
        athlete,
        ("fullName", "displayName", "name", "shortName"),
    )
    if not full_name:
        full_name = " ".join(
            value for value in (first_name, last_name) if value
        ).strip()
    if not full_name:
        return None

    raw = dict(athlete)
    raw.update(
        {
            "cffSource": "espn",
            "cffScope": SCOPE,
            "cffTeam": team.school,
            "cffConference": team.conference,
            "cffSeason": season,
            "espnTeamId": team.id,
        }
    )
    return Player(
        id=athlete_id,
        full_name=full_name,
        first_name=first_name,
        last_name=last_name,
        position=_position(athlete),
        team=team.school,
        conference=team.conference,
        year=_class_year(athlete),
        height=_height(athlete),
        weight=_weight(athlete),
        season=season,
        raw=raw,
    )


def parse_roster(payload: Mapping[str, Any], team: Team, season: int) -> list[Player]:
    players: dict[str, Player] = {}
    for athlete in _roster_items(payload):
        player = parse_player(athlete, team, season)
        if player is not None:
            players[player.id] = player
    return sorted(players.values(), key=lambda item: item.full_name.casefold())


def _load_psycopg():
    try:
        import psycopg  # type: ignore[import-not-found]
    except ImportError as error:
        raise IngestError("The runtime image must include psycopg") from error
    return psycopg


def completed_team_ids(database_url: str, season: int) -> set[str]:
    psycopg = _load_psycopg()
    with psycopg.connect(database_url) as connection:
        with connection.cursor() as cursor:
            cursor.execute(
                """
                SELECT team_id
                FROM espn_roster_progress
                WHERE season = %s
                  AND scope = %s
                  AND status = ANY(%s)
                """,
                (season, SCOPE, list(TERMINAL_PROGRESS_STATUSES)),
            )
            return {str(row[0]) for row in cursor.fetchall()}


def record_progress(
    database_url: str,
    team: Team,
    season: int,
    status: str,
    player_count: int,
    error_message: str | None,
) -> None:
    psycopg = _load_psycopg()
    with psycopg.connect(database_url) as connection:
        with connection.cursor() as cursor:
            cursor.execute(
                """
                INSERT INTO espn_roster_progress (
                    season, scope, team_id, team_name, conference,
                    status, player_count, error_message, updated_at
                )
                VALUES (%s, %s, %s, %s, %s, %s, %s, %s, NOW())
                ON CONFLICT (season, scope, team_id) DO UPDATE SET
                    team_name = EXCLUDED.team_name,
                    conference = EXCLUDED.conference,
                    status = EXCLUDED.status,
                    player_count = EXCLUDED.player_count,
                    error_message = EXCLUDED.error_message,
                    updated_at = NOW()
                """,
                (
                    season,
                    SCOPE,
                    team.id,
                    team.school,
                    team.conference,
                    status,
                    player_count,
                    error_message,
                ),
            )


def upsert_team_players(
    database_url: str,
    team: Team,
    players: Sequence[Player],
    season: int,
) -> tuple[int, int]:
    psycopg = _load_psycopg()
    inserted = 0
    updated = 0
    with psycopg.connect(database_url) as connection:
        with connection.cursor() as cursor:
            cursor.execute("ALTER TABLE players ADD COLUMN IF NOT EXISTS season INTEGER")
            cursor.execute(
                "ALTER TABLE players ADD COLUMN IF NOT EXISTS active BOOLEAN NOT NULL DEFAULT TRUE"
            )
            cursor.execute(
                "ALTER TABLE players ADD COLUMN IF NOT EXISTS last_seen_at TIMESTAMPTZ NOT NULL DEFAULT NOW()"
            )
            for player in players:
                cursor.execute(
                    """
                    INSERT INTO players (
                        id, full_name, first_name, last_name, position, team,
                        conference, year, height, weight, season, active,
                        last_seen_at, raw
                    ) VALUES (
                        %s, %s, NULLIF(%s, ''), NULLIF(%s, ''), NULLIF(%s, ''),
                        NULLIF(%s, ''), NULLIF(%s, ''), NULLIF(%s, ''),
                        NULLIF(%s, ''), %s, %s, TRUE, NOW(), %s::jsonb
                    )
                    ON CONFLICT (id) DO UPDATE SET
                        full_name = EXCLUDED.full_name,
                        first_name = EXCLUDED.first_name,
                        last_name = EXCLUDED.last_name,
                        position = EXCLUDED.position,
                        team = EXCLUDED.team,
                        conference = EXCLUDED.conference,
                        year = EXCLUDED.year,
                        height = EXCLUDED.height,
                        weight = EXCLUDED.weight,
                        season = EXCLUDED.season,
                        active = TRUE,
                        last_seen_at = NOW(),
                        raw = EXCLUDED.raw,
                        updated_at = NOW()
                    RETURNING (xmax = 0) AS inserted
                    """,
                    (
                        player.id,
                        player.full_name,
                        player.first_name,
                        player.last_name,
                        player.position,
                        player.team,
                        player.conference,
                        player.year,
                        player.height,
                        player.weight,
                        player.season,
                        json.dumps(player.raw, separators=(",", ":")),
                    ),
                )
                row = cursor.fetchone()
                if row and bool(row[0]):
                    inserted += 1
                else:
                    updated += 1
            cursor.execute(
                """
                INSERT INTO espn_roster_progress (
                    season, scope, team_id, team_name, conference,
                    status, player_count, error_message, updated_at
                )
                VALUES (%s, %s, %s, %s, %s, 'success', %s, NULL, NOW())
                ON CONFLICT (season, scope, team_id) DO UPDATE SET
                    team_name = EXCLUDED.team_name,
                    conference = EXCLUDED.conference,
                    status = 'success',
                    player_count = EXCLUDED.player_count,
                    error_message = NULL,
                    updated_at = NOW()
                """,
                (
                    season,
                    SCOPE,
                    team.id,
                    team.school,
                    team.conference,
                    len(players),
                ),
            )
    return inserted, updated


def finalize_import(
    database_url: str,
    season: int,
    call_count: int,
) -> tuple[int, int, int]:
    psycopg = _load_psycopg()
    with psycopg.connect(database_url) as connection:
        with connection.cursor() as cursor:
            cursor.execute(
                """
                UPDATE players
                SET active = FALSE, updated_at = NOW()
                WHERE season = %s
                  AND raw->>'cffSource' = 'espn'
                  AND COALESCE(raw->>'cffScope', '') <> %s
                """,
                (season, SCOPE),
            )
            cursor.execute(
                """
                SELECT
                    COUNT(*) FILTER (WHERE status = 'success'),
                    COUNT(*) FILTER (WHERE status = 'empty')
                FROM espn_roster_progress
                WHERE season = %s AND scope = %s
                """,
                (season, SCOPE),
            )
            progress_row = cursor.fetchone() or (0, 0)
            successful_teams = int(progress_row[0] or 0)
            empty_teams = int(progress_row[1] or 0)
            cursor.execute(
                """
                SELECT COUNT(*)
                FROM players
                WHERE season = %s
                  AND active = TRUE
                  AND raw->>'cffSource' = 'espn'
                  AND raw->>'cffScope' = %s
                """,
                (season, SCOPE),
            )
            player_row = cursor.fetchone()
            player_count = int(player_row[0] if player_row else 0)
            cursor.execute(
                """
                INSERT INTO ingestion_runs (
                    resource, season, finished_at, status, call_count,
                    row_count, error_message
                ) VALUES (%s, %s, NOW(), 'success', %s, %s, NULL)
                """,
                (RESOURCE, season, call_count, player_count),
            )
    return successful_teams, empty_teams, player_count


def write_export(
    path: Path,
    teams: Sequence[Team],
    players: Sequence[Player],
    season: int,
) -> None:
    payload = {
        "source": "espn",
        "scope": SCOPE,
        "season": season,
        "generatedAt": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "teams": [asdict(team) for team in teams],
        "playersFetchedThisRun": [asdict(player) for player in players],
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(payload, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Fetch ESPN Division I FBS rosters and upsert them into Postgres."
    )
    parser.add_argument(
        "--season",
        type=int,
        default=int(os.environ.get("ESPN_ROSTER_SEASON", "2026")),
    )
    parser.add_argument("--database-url", default=os.environ.get("DB_URL", ""))
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("espn-fbs-rosters-2026.json"),
    )
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--retries", type=int, default=4)
    parser.add_argument("--delay", type=float, default=0.2)
    parser.add_argument("--allow-partial", action="store_true")
    parser.add_argument("--allow-unexpected-team-count", action="store_true")
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    if args.season < 2000 or args.season > 2100:
        raise IngestError("--season must be between 2000 and 2100")
    if args.timeout <= 0 or args.retries <= 0 or args.delay < 0:
        raise IngestError("timeout/retries must be positive and delay cannot be negative")
    if not args.dry_run and not args.database_url:
        raise IngestError("DB_URL or --database-url is required unless --dry-run is used")

    teams, team_calls = fetch_fbs_teams(
        timeout=args.timeout,
        retries=args.retries,
        allow_unexpected_team_count=args.allow_unexpected_team_count,
    )
    completed = (
        set() if args.dry_run else completed_team_ids(args.database_url, args.season)
    )
    if completed:
        print(
            f"Resuming FBS import: {len(completed)} teams already completed; "
            f"{len(teams) - len(completed)} remain.",
            flush=True,
        )

    calls = team_calls
    fetched_players: list[Player] = []
    failures: list[str] = []
    inserted = 0
    updated = 0
    processed = 0

    remaining_teams = [team for team in teams if team.id not in completed]
    for index, team in enumerate(remaining_teams, start=1):
        url = f"{ESPN_BASE_URL}/teams/{urllib.parse.quote(team.id)}/roster"
        try:
            payload = fetch_json(url, timeout=args.timeout, retries=args.retries)
            calls += 1
            roster = parse_roster(payload, team, args.season)
            fetched_players.extend(roster)
            processed += 1
            if not roster:
                if not args.dry_run:
                    record_progress(
                        args.database_url,
                        team,
                        args.season,
                        "empty",
                        0,
                        None,
                    )
                print(
                    f"[{index}/{len(remaining_teams)}] {team.school}: empty roster; checkpointed",
                    file=sys.stderr,
                    flush=True,
                )
            else:
                if not args.dry_run:
                    team_inserted, team_updated = upsert_team_players(
                        args.database_url,
                        team,
                        roster,
                        args.season,
                    )
                    inserted += team_inserted
                    updated += team_updated
                print(
                    f"[{index}/{len(remaining_teams)}] {team.school}: "
                    f"{len(roster)} players; checkpointed",
                    flush=True,
                )
        except IngestError as error:
            calls += 1
            failures.append(f"{team.school} ({error})")
            if not args.dry_run:
                record_progress(
                    args.database_url,
                    team,
                    args.season,
                    "failed",
                    0,
                    str(error),
                )
            print(
                f"[{index}/{len(remaining_teams)}] {team.school}: {error}",
                file=sys.stderr,
                flush=True,
            )
        if args.delay > 0 and index < len(remaining_teams):
            time.sleep(args.delay)

    write_export(args.output, teams, fetched_players, args.season)

    if args.dry_run:
        print(
            f"Dry run complete: fetched {len(fetched_players)} players from "
            f"{processed} teams; Postgres was not changed.",
            flush=True,
        )
        return 0

    if failures and not args.allow_partial:
        raise IngestError(
            f"{len(failures)} FBS team roster requests failed. Completed teams were "
            "checkpointed; the next deployment will retry only failed teams."
        )

    successful_teams, empty_teams, player_count = finalize_import(
        args.database_url,
        args.season,
        calls,
    )
    if player_count <= 0:
        raise IngestError("No active ESPN FBS players exist after import")

    print(
        json.dumps(
            {
                "status": "success",
                "source": "espn",
                "scope": SCOPE,
                "season": args.season,
                "teamsDiscovered": len(teams),
                "teamsAlreadyCompleted": len(completed),
                "teamsProcessedThisRun": processed,
                "teamsImported": successful_teams,
                "teamsEmpty": empty_teams,
                "teamsFailed": len(failures),
                "players": player_count,
                "insertedThisRun": inserted,
                "updatedThisRun": updated,
                "apiCallsThisRun": calls,
                "partial": bool(failures),
            },
            indent=2,
        ),
        flush=True,
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except IngestError as error:
        print(
            json.dumps({"status": "failed", "error": str(error)}, indent=2),
            file=sys.stderr,
            flush=True,
        )
        raise SystemExit(1)
