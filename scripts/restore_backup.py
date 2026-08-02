#!/usr/bin/env python3
"""Restore an off-platform PostgreSQL backup into an explicitly disposable database."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import subprocess
import tempfile
import urllib.parse
from datetime import datetime, timezone
from typing import Any

import boto3

from release_gate_common import GateFailure, add_check, enforce_checks, env_flag, int_env, require_env, utc_now, write_failure_report, write_report


EXPECTED_TABLES = (
    "users",
    "leagues",
    "league_members",
    "players",
    "rosters",
    "draft_picks",
    "waiver_claims",
    "trade_offers",
    "transactions",
    "league_matchups",
    "ingestion_runs",
)


def parse_checksum(value: str) -> str:
    digest = value.strip().split()[0] if value.strip() else ""
    if len(digest) != 64 or any(character not in "0123456789abcdefABCDEF" for character in digest):
        raise GateFailure("Backup checksum sidecar did not contain a valid SHA-256 digest")
    return digest.lower()


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def database_identity(url: str) -> tuple[str, str, int | None, str]:
    parsed = urllib.parse.urlsplit(url)
    return (
        parsed.scheme.lower(),
        (parsed.hostname or "").lower(),
        parsed.port,
        urllib.parse.unquote(parsed.path or ""),
    )


def run(command: list[str], *, env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(command, check=True, text=True, capture_output=True, env=env)
    except FileNotFoundError as exc:
        raise GateFailure(f"Required command is unavailable: {command[0]}") from exc
    except subprocess.CalledProcessError as exc:
        detail = (exc.stderr or exc.stdout or str(exc)).strip()
        raise GateFailure(f"Command failed ({command[0]}): {detail}") from exc


def psql_value(db_url: str, sql: str) -> str:
    result = run(
        ["psql", db_url, "--no-psqlrc", "--tuples-only", "--no-align", "--set", "ON_ERROR_STOP=1", "--command", sql],
        env={**os.environ, "PGCONNECT_TIMEOUT": "30"},
    )
    return result.stdout.strip()


def table_exists(db_url: str, table: str) -> bool:
    return psql_value(db_url, f"SELECT to_regclass('public.{table}') IS NOT NULL;").lower() == "t"


def table_count(db_url: str, table: str) -> int | None:
    if not table_exists(db_url, table):
        return None
    raw = psql_value(db_url, f'SELECT COUNT(*) FROM public."{table}";')
    return int(raw or 0)


def select_latest_backup(client: Any, bucket: str, prefix: str) -> str:
    latest_key = ""
    latest_modified: datetime | None = None
    paginator = client.get_paginator("list_objects_v2")
    for page in paginator.paginate(Bucket=bucket, Prefix=f"{prefix}/"):
        for item in page.get("Contents", []):
            key = str(item.get("Key", ""))
            modified = item.get("LastModified")
            if not key.endswith(".dump") or modified is None:
                continue
            if latest_modified is None or modified > latest_modified:
                latest_key = key
                latest_modified = modified
    if not latest_key:
        raise GateFailure(f"No .dump backup objects were found under s3://{bucket}/{prefix}/")
    return latest_key


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--key", help="Exact backup object key. The latest .dump under the prefix is used by default.")
    args = parser.parse_args()

    if os.environ.get("CFF_RESTORE_CONFIRM", "").strip() != "restore-disposable-database":
        raise GateFailure("Set CFF_RESTORE_CONFIRM=restore-disposable-database to acknowledge the destructive restore target")

    target_db = require_env("CFF_RESTORE_TARGET_DB_URL")
    source_db = os.environ.get("DB_URL", "").strip()
    if source_db and database_identity(source_db) == database_identity(target_db):
        raise GateFailure("CFF_RESTORE_TARGET_DB_URL must not point at the source/production DB_URL")

    bucket = require_env("CFF_BACKUP_S3_BUCKET")
    access_key = require_env("AWS_ACCESS_KEY_ID")
    secret_key = require_env("AWS_SECRET_ACCESS_KEY")
    region = os.environ.get("AWS_REGION", "auto").strip() or "auto"
    endpoint = os.environ.get("AWS_ENDPOINT_URL", "").strip() or None
    prefix = os.environ.get("CFF_BACKUP_S3_PREFIX", "college-ff/postgres").strip().strip("/") or "college-ff/postgres"
    allow_nonempty = env_flag("CFF_RESTORE_ALLOW_NONEMPTY", False)
    min_players = int_env("CFF_RESTORE_MIN_PLAYERS", 1, 0)
    report_dir = os.environ.get("CFF_RELEASE_GATE_REPORT_DIR", "release-gate-artifacts")

    session = boto3.session.Session(
        aws_access_key_id=access_key,
        aws_secret_access_key=secret_key,
        region_name=region,
    )
    client = session.client("s3", endpoint_url=endpoint)
    key = args.key or os.environ.get("CFF_RESTORE_BACKUP_KEY", "").strip() or select_latest_backup(client, bucket, prefix)
    checksum_key = f"{key}.sha256"
    checks: list[dict[str, Any]] = []
    evidence: dict[str, Any] = {"bucket": bucket, "key": key, "checksumKey": checksum_key}

    current_db = psql_value(target_db, "SELECT current_database();")
    existing_tables = int(
        psql_value(
            target_db,
            "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema='public' AND table_type='BASE TABLE';",
        )
        or 0
    )
    add_check(checks, "Disposable target database reachable", bool(current_db), f"database={current_db}")
    add_check(checks, "Restore target safety", existing_tables == 0 or allow_nonempty, f"existingPublicTables={existing_tables}; allowNonempty={allow_nonempty}")
    if existing_tables and not allow_nonempty:
        report = {
            "title": "Backup restore validation",
            "generatedAt": utc_now(),
            "status": "failed",
            "classification": "restore-blocked",
            "checks": checks,
            "evidence": evidence,
        }
        write_report(report, report_dir, "backup-restore-validation")
        enforce_checks(checks)

    with tempfile.TemporaryDirectory(prefix="cff-restore-") as temp_dir:
        temp_path = pathlib.Path(temp_dir)
        dump_path = temp_path / pathlib.Path(key).name
        checksum_path = temp_path / pathlib.Path(checksum_key).name
        client.download_file(bucket, key, str(dump_path))
        client.download_file(bucket, checksum_key, str(checksum_path))
        expected_digest = parse_checksum(checksum_path.read_text(encoding="utf-8"))
        actual_digest = sha256_file(dump_path)
        object_head = client.head_object(Bucket=bucket, Key=key)
        object_size = int(object_head.get("ContentLength", 0) or 0)
        metadata_digest = str(object_head.get("Metadata", {}).get("sha256", "")).lower()
        add_check(checks, "Backup object is nonempty", object_size > 0 and dump_path.stat().st_size == object_size, f"bytes={object_size}")
        add_check(checks, "Backup checksum matches sidecar", actual_digest == expected_digest, f"sha256={actual_digest}")
        add_check(checks, "Backup metadata checksum matches", not metadata_digest or metadata_digest == actual_digest, f"metadataSha256={metadata_digest or 'not-set'}")

        archive = run(["pg_restore", "--list", str(dump_path)])
        entries = [line for line in archive.stdout.splitlines() if line and not line.startswith(";")]
        add_check(checks, "Backup archive is readable", len(entries) > 0, f"archiveEntries={len(entries)}")

        restore_command = [
            "pg_restore",
            "--no-owner",
            "--no-acl",
            "--exit-on-error",
            f"--dbname={target_db}",
        ]
        if existing_tables and allow_nonempty:
            restore_command.extend(["--clean", "--if-exists"])
        restore_command.append(str(dump_path))
        started = datetime.now(timezone.utc)
        run(restore_command, env={**os.environ, "PGCONNECT_TIMEOUT": "30"})
        duration = (datetime.now(timezone.utc) - started).total_seconds()
        evidence["restoreDurationSeconds"] = round(duration, 2)
        evidence["archiveEntries"] = len(entries)
        evidence["sha256"] = actual_digest

    counts: dict[str, int | None] = {table: table_count(target_db, table) for table in EXPECTED_TABLES}
    evidence["tableCounts"] = counts
    required_present = all(counts[table] is not None for table in ("users", "leagues", "players", "ingestion_runs"))
    add_check(checks, "Core application tables restored", required_present, json.dumps(counts, sort_keys=True))
    add_check(checks, "Player data restored", (counts.get("players") or 0) >= min_players, f"players={counts.get('players')}; minimum={min_players}")

    orphan_members = 0
    if counts.get("league_members") is not None and counts.get("leagues") is not None:
        orphan_members = int(
            psql_value(
                target_db,
                "SELECT COUNT(*) FROM league_members m LEFT JOIN leagues l ON l.id=m.league_id WHERE l.id IS NULL;",
            )
            or 0
        )
    orphan_rosters = 0
    if counts.get("rosters") is not None and counts.get("leagues") is not None:
        orphan_rosters = int(
            psql_value(
                target_db,
                "SELECT COUNT(*) FROM rosters r LEFT JOIN leagues l ON l.id=r.league_id WHERE l.id IS NULL;",
            )
            or 0
        )
    add_check(checks, "Restored league relations are consistent", orphan_members == 0 and orphan_rosters == 0, f"orphanMembers={orphan_members}; orphanRosters={orphan_rosters}")
    evidence["integrity"] = {"orphanMembers": orphan_members, "orphanRosters": orphan_rosters}

    passed = all(check["passed"] or not check["required"] for check in checks)
    report = {
        "title": "Backup restore validation",
        "generatedAt": utc_now(),
        "status": "passed" if passed else "failed",
        "classification": "restore-ready" if passed else "restore-blocked",
        "checks": checks,
        "evidence": evidence,
        "summary": "The selected off-platform object and checksum were downloaded, cryptographically verified, inspected with pg_restore, restored into an explicitly disposable database, and checked for core tables, player population, and orphaned league relations.",
    }
    json_path, _ = write_report(report, report_dir, "backup-restore-validation")
    report_digest = sha256_file(json_path)
    (json_path.parent / "backup-restore-validation.evidence.sha256").write_text(f"{report_digest}  {json_path.name}\n", encoding="utf-8")
    enforce_checks(checks)
    print(json.dumps({"status": "ok", "key": key, "evidenceSha256": report_digest, "counts": counts}, indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        write_failure_report("Backup restore validation", "backup-restore-validation", exc)
        print(json.dumps({"status": "failed", "error": str(exc)}, indent=2), file=os.sys.stderr)
        raise SystemExit(1)
