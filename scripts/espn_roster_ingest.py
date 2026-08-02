#!/usr/bin/env python3
"""One-time ESPN roster bootstrap for every college-football team ESPN returns."""

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

from espn_team_directory import all_teams_url

ESPN_BASE_URL = (
    "https://site.api.espn.com/apis/site/v2/sports/football/college-football"
)
USER_AGENT = "CollegeFantasyFootball-roster-bootstrap/1.1"
EXPECTED_ALL_TEAM_RANGE = range(100, 1001)


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
                print(f"ESPN HTTP {error.code}; retrying in {delay:.1f}s", file=sys.stderr)
                time.sleep(delay)
        except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as error:
            last_error = error
            if attempt < retries:
                delay = _retry_delay({}, attempt)
                print(f"ESPN request error; retrying in {delay:.1f}s: {error}", file=sys.stderr)
                time.sleep(delay)
    raise IngestError(f"ESPN request failed after {retries} attempts: {url}: {last_error}")


def _team_entries(payload: Mapping[str, Any]) -> list[Mapping[str, Any]]:
    entries: list[Mapping[str, Any]] = []
    sports = payload.get("sports")
    if not isinstance(sports, list):
        return entries
    for sport in sports:
        if not isinstance(sport, Mapping):
            continue
        leagues = sport.get("leagues")
        if not isinstance(leagues, list):
            continue
        for league in leagues:
            if not isinstance(league, Mapping):
                continue
            teams = league.get("teams")
            if isinstance(teams, list):
                entries.extend(item for item in teams if isinstance(item, Mapping))
    return entries


def parse_teams(payload: Mapping[str, Any], conference: str = "NCAA") -> list[Team]:
    teams: dict[str, Team] = {}
    for entry in _team_entries(payload):
        value = entry.get("team")
        team = value if isinstance(value, Mapping) else entry
        team_id = _first_text(team, ("id", "uid"))
        school = _first_text(team, ("location", "shortDisplayName", "displayName", "name"))
        if not team_id or not school:
            continue
        teams[team_id] = Team(
            id=team_id,
            school=school,
            conference=conference,
            slug=_first_text(team, ("slug", "abbreviation")),
        )
    return sorted(teams.values(), key=lambda item: item.school.casefold())


def fetch_all_teams(
    *, timeout: float, retries: int, allow_unexpected_team_count: bool
) -> tuple[list[Team], int]:
    payload = fetch_json(all_teams_url(), timeout=timeout, retries=retries)
    teams = parse_teams(payload)
    if len(teams) not in EXPECTED_ALL_TEAM_RANGE and not allow_unexpected_team_count:
        raise IngestError(
            f"ESPN returned {len(teams)} total teams; expected 100-1000. "
            "Use --allow-unexpected-team-count only after inspecting the response."
        )
    print(f"Found {len(teams)} total ESPN college-football teams.", flush=True)
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
            text = _first_text(value, ("abbreviation", "displayValue", "name", "shortName", "years"))
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


def parse_player(athlete: Mapping[str, Any], team: Team, season: int) -> Player | None:
    athlete_id = _first_text(athlete, ("id", "uid"))
    if not athlete_id:
        return None
    first_name = _first_text(athlete, ("firstName", "first_name"))
    last_name = _first_text(athlete, ("lastName", "last_name"))
    full_name = _first_text(athlete, ("fullName", "displayName", "name", "shortName"))
    if not full_name:
        full_name = " ".join(value for value in (first_name, last_name) if value).strip()
    if not full_name:
        return None
    raw = dict(athlete)
    raw.update(
        {
            "cffSource": "espn",
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


def fetch_rosters(
    teams: Sequence[Team], *, season: int, timeout: float, retries: int, delay: float
) -> tuple[list[Player], int, list[str], int]:
    players: dict[str, Player] = {}
    failures: list[str] = []
    successful_teams = 0
    calls = 0
    for index, team in enumerate(teams, start=1):
        url = f"{ESPN_BASE_URL}/teams/{urllib.parse.quote(team.id)}/roster"
        try:
            payload = fetch_json(url, timeout=timeout, retries=retries)
            calls += 1
            roster = [
                player
                for athlete in _roster_items(payload)
                if (player := parse_player(athlete, team, season)) is not None
            ]
            if not roster:
                failures.append(f"{team.school} (empty roster)")
                print(f"[{index}/{len(teams)}] {team.school}: empty roster", file=sys.stderr, flush=True)
            else:
                successful_teams += 1
                for player in roster:
                    players[player.id] = player
                print(f"[{index}/{len(teams)}] {team.school}: {len(roster)} players", flush=True)
        except IngestError as error:
            calls += 1
            failures.append(f"{team.school} ({error})")
            print(f"[{index}/{len(teams)}] {team.school}: {error}", file=sys.stderr, flush=True)
        if delay > 0 and index < len(teams):
            time.sleep(delay)
    ordered = sorted(players.values(), key=lambda item: (item.team, item.full_name))
    return ordered, calls, failures, successful_teams


def write_export(path: Path, teams: Sequence[Team], players: Sequence[Player], season: int) -> None:
    payload = {
        "source": "espn",
        "scope": "all-college-football-teams",
        "season": season,
        "generatedAt": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "teams": [asdict(team) for team in teams],
        "players": [asdict(player) for player in players],
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def upsert_players(database_url: str, players: Sequence[Player], season: int, call_count: int) -> tuple[int, int]:
    try:
        import psycopg  # type: ignore[import-not-found]
    except ImportError as error:
        raise IngestError("The runtime image must include psycopg") from error
    inserted = 0
    updated = 0
    with psycopg.connect(database_url) as connection:
        with connection.cursor() as cursor:
            cursor.execute("ALTER TABLE players ADD COLUMN IF NOT EXISTS season INTEGER")
            cursor.execute("ALTER TABLE players ADD COLUMN IF NOT EXISTS active BOOLEAN NOT NULL DEFAULT TRUE")
            cursor.execute("ALTER TABLE players ADD COLUMN IF NOT EXISTS last_seen_at TIMESTAMPTZ NOT NULL DEFAULT NOW()")
            for player in players:
                cursor.execute(
                    """
                    INSERT INTO players (
                        id, full_name, first_name, last_name, position, team,
                        conference, year, height, weight, season, active, last_seen_at, raw
                    ) VALUES (
                        %s, %s, NULLIF(%s, ''), NULLIF(%s, ''), NULLIF(%s, ''),
                        NULLIF(%s, ''), NULLIF(%s, ''), NULLIF(%s, ''), NULLIF(%s, ''),
                        %s, %s, TRUE, NOW(), %s::jsonb
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
                        player.id, player.full_name, player.first_name, player.last_name,
                        player.position, player.team, player.conference, player.year,
                        player.height, player.weight, player.season,
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
                INSERT INTO ingestion_runs (
                    resource, season, finished_at, status, call_count, row_count, error_message
                ) VALUES ('players_espn', %s, NOW(), 'success', %s, %s, NULL)
                """,
                (season, call_count, inserted + updated),
            )
    return inserted, updated


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Fetch all ESPN college-football rosters and upsert them into Postgres.")
    parser.add_argument("--season", type=int, default=int(os.environ.get("ESPN_ROSTER_SEASON", "2026")))
    parser.add_argument("--database-url", default=os.environ.get("DB_URL", ""))
    parser.add_argument("--output", type=Path, default=Path("espn-rosters-2026.json"))
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--retries", type=int, default=4)
    parser.add_argument("--delay", type=float, default=0.15)
    parser.add_argument("--allow-partial", action="store_true")
    parser.add_argument("--allow-unexpected-team-count", action="store_true")
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    if args.season < 2000 or args.season > 2100:
        raise IngestError("--season must be between 2000 and 2100")
    if args.timeout <= 0 or args.retries <= 0 or args.delay < 0:
        raise IngestError("timeout/retries must be positive and delay cannot be negative")

    teams, team_calls = fetch_all_teams(
        timeout=args.timeout,
        retries=args.retries,
        allow_unexpected_team_count=args.allow_unexpected_team_count,
    )
    players, roster_calls, failed_teams, successful_teams = fetch_rosters(
        teams,
        season=args.season,
        timeout=args.timeout,
        retries=args.retries,
        delay=args.delay,
    )
    total_calls = team_calls + roster_calls
    write_export(args.output, teams, players, args.season)
    print(
        f"Wrote {len(players)} players from {successful_teams}/{len(teams)} teams to {args.output}",
        flush=True,
    )

    if failed_teams:
        print(f"Skipped {len(failed_teams)} teams with unavailable or empty rosters.", file=sys.stderr, flush=True)
    if not players or successful_teams == 0:
        raise IngestError("ESPN returned no usable rosters; Postgres was not changed")
    if args.dry_run:
        print("Dry run complete; Postgres was not changed.", flush=True)
        return 0
    if not args.database_url:
        raise IngestError("DB_URL or --database-url is required unless --dry-run is used")

    inserted, updated = upsert_players(args.database_url, players, args.season, total_calls)
    print(
        json.dumps(
            {
                "status": "success",
                "source": "espn",
                "scope": "all-college-football-teams",
                "season": args.season,
                "teamsDiscovered": len(teams),
                "teamsImported": successful_teams,
                "teamsSkipped": len(failed_teams),
                "players": len(players),
                "inserted": inserted,
                "updated": updated,
                "apiCalls": total_calls,
                "partial": bool(failed_teams),
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
        print(json.dumps({"status": "failed", "error": str(error)}, indent=2), file=sys.stderr, flush=True)
        raise SystemExit(1)
