#!/usr/bin/env python3
"""Run the ESPN all-team roster bootstrap once across all future deployments.

The Render pre-deploy hook invokes this file on every deploy. A successful
`players_espn` ingestion ledger row permanently disables later ESPN fetches.
A PostgreSQL advisory lock prevents overlapping deploys from starting duplicate
imports.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path


class BootstrapError(RuntimeError):
    """Raised when the automatic one-time bootstrap cannot run safely."""


def env_flag(name: str, default: bool = False) -> bool:
    raw = os.environ.get(name)
    if raw is None:
        return default
    return raw.strip().lower() in {"1", "true", "yes", "on"}


def configured_season() -> int:
    raw = os.environ.get("ESPN_ROSTER_SEASON", "2026").strip()
    try:
        season = int(raw)
    except ValueError as error:
        raise BootstrapError("ESPN_ROSTER_SEASON must be a four-digit year") from error
    if season < 2000 or season > 2100:
        raise BootstrapError("ESPN_ROSTER_SEASON must be between 2000 and 2100")
    return season


def main() -> int:
    if not env_flag("ESPN_ROSTER_AUTO_ONCE", default=True):
        print("[espn-bootstrap] automatic one-time import is disabled", flush=True)
        return 0

    database_url = os.environ.get("DB_URL", "").strip()
    if not database_url:
        raise BootstrapError("DB_URL is required for the automatic ESPN bootstrap")

    try:
        import psycopg  # type: ignore[import-not-found]
    except ImportError as error:
        raise BootstrapError(
            "The runtime image must include the Python psycopg package"
        ) from error

    season = configured_season()
    lock_name = "cff:espn-roster-bootstrap"

    with psycopg.connect(database_url, autocommit=True) as connection:
        with connection.cursor() as cursor:
            cursor.execute(
                "SELECT pg_try_advisory_lock(hashtext(%s)::bigint)",
                (lock_name,),
            )
            row = cursor.fetchone()
            if not row or not bool(row[0]):
                print(
                    "[espn-bootstrap] another deployment is already running the one-time import; skipping",
                    flush=True,
                )
                return 0

            cursor.execute(
                """
                SELECT EXISTS (
                    SELECT 1
                    FROM ingestion_runs
                    WHERE resource = 'players_espn'
                      AND status = 'success'
                      AND COALESCE(row_count, 0) > 0
                )
                """
            )
            completed = cursor.fetchone()
            if completed and bool(completed[0]):
                print(
                    "[espn-bootstrap] a successful ESPN roster import already exists; no ESPN requests were made",
                    flush=True,
                )
                return 0

            importer = Path(__file__).with_name("espn_roster_ingest.py")
            if not importer.is_file():
                raise BootstrapError(f"ESPN importer not found at {importer}")

            output_path = Path("/tmp") / f"espn-rosters-{season}.json"
            command = [
                sys.executable,
                str(importer),
                "--season",
                str(season),
                "--output",
                str(output_path),
            ]
            if env_flag("ESPN_ROSTER_ALLOW_PARTIAL"):
                command.append("--allow-partial")
            if env_flag("ESPN_ROSTER_ALLOW_UNEXPECTED_TEAM_COUNT"):
                command.append("--allow-unexpected-team-count")

            print(
                f"[espn-bootstrap] no successful prior import found; starting one-time {season} all-team import",
                flush=True,
            )
            subprocess.run(command, check=True)
            print(
                "[espn-bootstrap] import completed successfully; future deployments will skip it",
                flush=True,
            )
            return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (BootstrapError, subprocess.CalledProcessError) as error:
        print(
            json.dumps({"status": "failed", "error": str(error)}, indent=2),
            file=sys.stderr,
            flush=True,
        )
        raise SystemExit(1)
