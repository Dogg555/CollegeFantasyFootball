#!/usr/bin/env python3
"""
CollegeFootballData (CFBD) ingestion helper.

Pulls teams and rosters from the public CFBD API and upserts them into the
existing Postgres schema (teams, players).

Configuration via CLI flags or environment variables:
  --api-key / CFBD_API_KEY   - API key for https://collegefootballdata.com
  --db-url / DB_URL          - Postgres connection string (e.g., postgres://user:pass@host:5432/db)
  --season / CFBD_SEASON     - Season year to ingest (defaults to current year)
  --delay / CFBD_DELAY_SEC   - Optional delay between HTTP calls (default: 0.2)
"""

import argparse
import datetime
import json
import os
import sys
import time
from typing import Dict, Iterable, List, Tuple

import psycopg2
import psycopg2.extras
import requests

BASE_URL = "https://api.collegefootballdata.com"
PROVIDER_ID = 1  # reserved provider_id for CFBD in our schema


class IngestError(Exception):
    """Domain error for ingestion failures."""


def fetch_json(api_key: str, path: str, params: Dict) -> List[Dict]:
    url = f"{BASE_URL}{path}"
    headers = {
        "Authorization": f"Bearer {api_key}",
        "Accept": "application/json",
        "User-Agent": "cff-ingestor/1.0 (+github.com/CollegeFantasyFootball)",
    }
    resp = requests.get(url, headers=headers, params=params, timeout=30)
    try:
        resp.raise_for_status()
    except Exception as exc:  # pragma: no cover - thin wrapper
        raise IngestError(f"Request failed for {url} {params}: {exc}") from exc
    try:
        return resp.json()
    except ValueError as exc:  # pragma: no cover
        raise IngestError(f"Invalid JSON from {url}: {resp.text[:200]}") from exc


def location_from_team(team: Dict) -> str:
    city = team.get("location") or team.get("city")
    state = team.get("state")
    if city and state:
        return f"{city}, {state}"
    return city or ""


def player_hometown(player: Dict) -> str:
    city = player.get("home_city")
    state = player.get("home_state")
    if city and state:
        return f"{city}, {state}"
    return city or ""


def player_full_name(player: Dict) -> str:
    first = (player.get("first_name") or "").strip()
    last = (player.get("last_name") or "").strip()
    if first and last:
        return f"{first} {last}"
    return player.get("name") or first or last or ""


def player_provider_id(team_provider_id: str, player: Dict) -> str:
    if player.get("id") is not None:
        return str(player["id"])
    name = player_full_name(player).replace(" ", "-") or "unknown"
    return f"{team_provider_id}-{name.lower()}"


def fetch_teams(api_key: str, season: int) -> List[Dict]:
    # Returns combined FBS and FCS teams for the season.
    teams = fetch_json(api_key, "/teams", {"year": season})
    return teams


def fetch_roster(api_key: str, season: int, school: str) -> List[Dict]:
    return fetch_json(api_key, "/roster", {"team": school, "year": season})


def upsert_teams(conn, teams: Iterable[Dict]) -> Dict[str, int]:
    """Insert/update teams; return map provider_team_id -> teams.id"""
    sql = """
    INSERT INTO teams (
        provider_id, provider_team_id, school, mascot, abbreviation, conference,
        division, location, raw_payload
    )
    VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s)
    ON CONFLICT (provider_id, provider_team_id) DO UPDATE
      SET school = EXCLUDED.school,
          mascot = EXCLUDED.mascot,
          abbreviation = EXCLUDED.abbreviation,
          conference = EXCLUDED.conference,
          division = EXCLUDED.division,
          location = EXCLUDED.location,
          raw_payload = EXCLUDED.raw_payload,
          ingested_at = now()
    RETURNING id, provider_team_id
    """
    rows = []
    for team in teams:
        provider_team_id = str(team.get("id") or team.get("school"))
        rows.append(
            (
                PROVIDER_ID,
                provider_team_id,
                team.get("school"),
                team.get("mascot"),
                team.get("abbreviation"),
                team.get("conference"),
                team.get("division"),
                location_from_team(team),
                json.dumps(team),
            )
        )

    team_map: Dict[str, int] = {}
    with conn.cursor() as cur:
        psycopg2.extras.execute_batch(cur, sql, rows, page_size=500)
        # fetchall after execute_batch returns results for the final statement only; rerun select.
        cur.execute(
            """
            SELECT id, provider_team_id
            FROM teams
            WHERE provider_id = %s
            """,
            (PROVIDER_ID,),
        )
        for row in cur.fetchall():
            team_map[row[1]] = row[0]
    conn.commit()
    return team_map


def upsert_players(conn, team_provider_id: str, team_id: int, roster: Iterable[Dict]) -> Tuple[int, int]:
    inserted = 0
    updated = 0
    sql = """
    INSERT INTO players (
        provider_id, provider_player_id, team_id, full_name, first_name, last_name,
        position, class, height_inches, weight_lbs, hometown, conference, raw_payload
    )
    VALUES (%s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s, %s)
    ON CONFLICT (provider_id, provider_player_id) DO UPDATE
      SET team_id = EXCLUDED.team_id,
          full_name = EXCLUDED.full_name,
          first_name = EXCLUDED.first_name,
          last_name = EXCLUDED.last_name,
          position = EXCLUDED.position,
          class = EXCLUDED.class,
          height_inches = EXCLUDED.height_inches,
          weight_lbs = EXCLUDED.weight_lbs,
          hometown = EXCLUDED.hometown,
          conference = EXCLUDED.conference,
          raw_payload = EXCLUDED.raw_payload,
          ingested_at = now()
    """

    rows = []
    for player in roster:
        pid = player_provider_id(team_provider_id, player)
        full_name = player_full_name(player)
        rows.append(
            (
                PROVIDER_ID,
                pid,
                team_id,
                full_name,
                (player.get("first_name") or "").strip(),
                (player.get("last_name") or "").strip(),
                player.get("position"),
                str(player.get("year") or player.get("classification") or ""),
                player.get("height"),
                player.get("weight"),
                player_hometown(player),
                player.get("team_conference") or player.get("conference"),
                json.dumps(player),
            )
        )

    with conn.cursor() as cur:
        psycopg2.extras.execute_batch(cur, sql, rows, page_size=500)
        cur.execute(
            """
            SELECT COUNT(*) FROM players WHERE provider_id = %s
            """,
            (PROVIDER_ID,),
        )
        # We can't distinguish inserts vs updates via execute_batch; return totals.
        total = cur.fetchone()[0]
    conn.commit()
    return inserted, total


def main() -> int:
    parser = argparse.ArgumentParser(description="CFBD ingestion helper (teams + rosters).")
    parser.add_argument(
        "--api-key",
        default=os.getenv("CFBD_API_KEY"),
        help="CFBD API key (env: CFBD_API_KEY)",
    )
    parser.add_argument(
        "--db-url",
        default=os.getenv("DB_URL"),
        help="Postgres connection string (env: DB_URL)",
    )
    parser.add_argument(
        "--season",
        type=int,
        default=int(os.getenv("CFBD_SEASON", datetime.date.today().year)),
        help="Season year to ingest (env: CFBD_SEASON; default: current year)",
    )
    parser.add_argument(
        "--delay",
        type=float,
        default=float(os.getenv("CFBD_DELAY_SEC", "0.2")),
        help="Delay between HTTP calls in seconds (env: CFBD_DELAY_SEC; default: 0.2)",
    )
    args = parser.parse_args()

    if not args.api_key:
        print("[ingest] configuration error: API key is required (use --api-key or CFBD_API_KEY)", file=sys.stderr)
        return 1
    if not args.db_url:
        print("[ingest] configuration error: DB URL is required (use --db-url or DB_URL)", file=sys.stderr)
        return 1

    conn = psycopg2.connect(args.db_url)
    try:
        print(f"[ingest] fetching teams for season {args.season} ...")
        teams = fetch_teams(args.api_key, args.season)
        team_map = upsert_teams(conn, teams)
        print(f"[ingest] upserted {len(team_map)} teams")

        total_players = 0
        for team in teams:
            provider_team_id = str(team.get("id") or team.get("school"))
            team_id = team_map.get(provider_team_id)
            if not team_id:
                print(f"[warn] no team_id for {team.get('school')}, skipping roster")
                continue
            roster = fetch_roster(args.api_key, args.season, team.get("school"))
            _, player_total_after = upsert_players(conn, provider_team_id, team_id, roster)
            total_players = player_total_after
            print(
                f"[ingest] roster for {team.get('school')}: {len(roster)} players (provider_id={provider_team_id})"
            )
            time.sleep(args.delay)

        print(f"[ingest] done. total teams={len(team_map)} total players={total_players}")
        return 0
    except IngestError as exc:
        print(f"[ingest] failed: {exc}", file=sys.stderr)
        return 1
    finally:
        conn.close()


if __name__ == "__main__":
    sys.exit(main())
