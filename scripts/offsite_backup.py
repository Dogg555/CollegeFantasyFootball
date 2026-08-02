#!/usr/bin/env python3
"""Create, validate, upload, verify, and age out PostgreSQL backups."""
from __future__ import annotations

import datetime as dt
import hashlib
import json
import os
import pathlib
import subprocess
import sys
import tempfile

import boto3


class BackupFailure(RuntimeError):
    pass


def require(name: str) -> str:
    value = os.environ.get(name, "").strip()
    if not value:
        raise BackupFailure(f"Missing required environment variable: {name}")
    return value


def env_flag(name: str, default: bool = False) -> bool:
    raw = os.environ.get(name)
    if raw is None:
        return default
    return raw.strip().lower() in {"1", "true", "yes", "on"}


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run(command: list[str], *, env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    try:
        return subprocess.run(command, check=True, text=True, capture_output=True, env=env)
    except FileNotFoundError as exc:
        raise BackupFailure(f"Required command is unavailable: {command[0]}") from exc
    except subprocess.CalledProcessError as exc:
        detail = (exc.stderr or exc.stdout or str(exc)).strip()
        raise BackupFailure(f"Command failed ({command[0]}): {detail}") from exc


def main() -> None:
    db_url = require("DB_URL")
    bucket = require("CFF_BACKUP_S3_BUCKET")
    access_key = require("AWS_ACCESS_KEY_ID")
    secret_key = require("AWS_SECRET_ACCESS_KEY")
    region = os.environ.get("AWS_REGION", "auto").strip() or "auto"
    endpoint = os.environ.get("AWS_ENDPOINT_URL", "").strip() or None
    prefix = os.environ.get("CFF_BACKUP_S3_PREFIX", "college-ff/postgres").strip().strip("/") or "college-ff/postgres"
    retention_days = int(os.environ.get("CFF_BACKUP_RETENTION_DAYS", "30"))
    min_bytes = int(os.environ.get("CFF_BACKUP_MIN_BYTES", "1024"))
    verify_upload = env_flag("CFF_BACKUP_VERIFY_UPLOAD", True)
    sse = os.environ.get("CFF_BACKUP_SSE", "AES256").strip()
    kms_key = os.environ.get("CFF_BACKUP_KMS_KEY_ID", "").strip()
    if retention_days < 7 or retention_days > 3650:
        raise BackupFailure("CFF_BACKUP_RETENTION_DAYS must be between 7 and 3650")
    if min_bytes < 1:
        raise BackupFailure("CFF_BACKUP_MIN_BYTES must be positive")

    session = boto3.session.Session(
        aws_access_key_id=access_key,
        aws_secret_access_key=secret_key,
        region_name=region,
    )
    client = session.client("s3", endpoint_url=endpoint)
    now = dt.datetime.now(dt.timezone.utc)
    timestamp = now.strftime("%Y%m%dT%H%M%SZ")
    key = f"{prefix}/college-ff-{timestamp}.dump"
    checksum_key = f"{key}.sha256"

    with tempfile.TemporaryDirectory(prefix="cff-backup-") as temp_dir:
        temp_path = pathlib.Path(temp_dir)
        dump_path = temp_path / "college-ff.dump"
        run(
            [
                "pg_dump",
                db_url,
                "--format=custom",
                "--compress=9",
                "--no-owner",
                "--no-acl",
                f"--file={dump_path}",
            ],
            env={**os.environ, "PGCONNECT_TIMEOUT": "30"},
        )
        size = dump_path.stat().st_size
        if size < min_bytes:
            raise BackupFailure(f"Backup dump is unexpectedly small: {size} bytes (minimum {min_bytes})")

        archive = run(["pg_restore", "--list", str(dump_path)])
        archive_entries = [line for line in archive.stdout.splitlines() if line and not line.startswith(";")]
        if not archive_entries:
            raise BackupFailure("pg_restore could not find any archive entries in the new dump")

        digest = sha256_file(dump_path)
        extra = {
            "Metadata": {
                "sha256": digest,
                "created-utc": now.isoformat(),
                "archive-entries": str(len(archive_entries)),
            },
            "ContentType": "application/octet-stream",
        }
        if sse:
            extra["ServerSideEncryption"] = sse
        if sse == "aws:kms" and kms_key:
            extra["SSEKMSKeyId"] = kms_key
        client.upload_file(str(dump_path), bucket, key, ExtraArgs=extra)
        sidecar_body = f"{digest}  {pathlib.Path(key).name}\n".encode("utf-8")
        client.put_object(
            Bucket=bucket,
            Key=checksum_key,
            Body=sidecar_body,
            ContentType="text/plain",
            **({"ServerSideEncryption": sse} if sse else {}),
            **({"SSEKMSKeyId": kms_key} if sse == "aws:kms" and kms_key else {}),
        )

        head = client.head_object(Bucket=bucket, Key=key)
        uploaded_size = int(head.get("ContentLength", 0) or 0)
        uploaded_digest = str(head.get("Metadata", {}).get("sha256", "")).lower()
        if uploaded_size != size:
            raise BackupFailure(f"Uploaded backup size mismatch: local={size} remote={uploaded_size}")
        if uploaded_digest and uploaded_digest != digest:
            raise BackupFailure("Uploaded backup metadata checksum does not match the local dump")

        sidecar = client.get_object(Bucket=bucket, Key=checksum_key)["Body"].read().decode("utf-8").strip().split()[0]
        if sidecar.lower() != digest:
            raise BackupFailure("Uploaded checksum sidecar does not match the local dump")

        upload_verified = False
        if verify_upload:
            downloaded_path = temp_path / "verified-download.dump"
            client.download_file(bucket, key, str(downloaded_path))
            downloaded_digest = sha256_file(downloaded_path)
            if downloaded_digest != digest:
                raise BackupFailure("Downloaded verification copy failed SHA-256 validation")
            downloaded_archive = run(["pg_restore", "--list", str(downloaded_path)])
            downloaded_entries = [line for line in downloaded_archive.stdout.splitlines() if line and not line.startswith(";")]
            if len(downloaded_entries) != len(archive_entries):
                raise BackupFailure("Downloaded verification copy has a different archive entry count")
            upload_verified = True

    cutoff = now - dt.timedelta(days=retention_days)
    deleted_count = 0
    pending = []
    paginator = client.get_paginator("list_objects_v2")
    for page in paginator.paginate(Bucket=bucket, Prefix=f"{prefix}/"):
        for item in page.get("Contents", []):
            object_key = item["Key"]
            if item["LastModified"] < cutoff and (
                object_key.endswith(".dump") or object_key.endswith(".dump.sha256")
            ):
                pending.append({"Key": object_key})
                if len(pending) == 1000:
                    client.delete_objects(Bucket=bucket, Delete={"Objects": pending, "Quiet": True})
                    deleted_count += len(pending)
                    pending.clear()
    if pending:
        client.delete_objects(Bucket=bucket, Delete={"Objects": pending, "Quiet": True})
        deleted_count += len(pending)

    print(json.dumps({
        "status": "ok",
        "bucket": bucket,
        "key": key,
        "checksumKey": checksum_key,
        "bytes": size,
        "sha256": digest,
        "archiveEntries": len(archive_entries),
        "uploadVerified": upload_verified,
        "retentionDays": retention_days,
        "deletedObjects": deleted_count,
        "serverSideEncryption": sse,
    }, indent=2))


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(json.dumps({"status": "failed", "error": str(exc)}, indent=2), file=sys.stderr)
        raise SystemExit(1)
