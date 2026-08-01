#!/usr/bin/env python3
"""Create, upload, and age out encrypted-at-rest PostgreSQL backups."""

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


def main() -> None:
    db_url = require("DB_URL")
    bucket = require("CFF_BACKUP_S3_BUCKET")
    access_key = require("AWS_ACCESS_KEY_ID")
    secret_key = require("AWS_SECRET_ACCESS_KEY")
    region = os.environ.get("AWS_REGION", "auto").strip() or "auto"
    endpoint = os.environ.get("AWS_ENDPOINT_URL", "").strip() or None
    prefix = os.environ.get("CFF_BACKUP_S3_PREFIX", "college-ff/postgres").strip().strip("/") or "college-ff/postgres"
    retention_days = int(os.environ.get("CFF_BACKUP_RETENTION_DAYS", "30"))
    sse = os.environ.get("CFF_BACKUP_SSE", "AES256").strip()
    kms_key = os.environ.get("CFF_BACKUP_KMS_KEY_ID", "").strip()
    if retention_days < 7 or retention_days > 3650:
        raise BackupFailure("CFF_BACKUP_RETENTION_DAYS must be between 7 and 3650")

    session = boto3.session.Session(
        aws_access_key_id=access_key,
        aws_secret_access_key=secret_key,
        region_name=region,
    )
    client = session.client("s3", endpoint_url=endpoint)
    now = dt.datetime.now(dt.timezone.utc)
    timestamp = now.strftime("%Y%m%dT%H%M%SZ")
    key = f"{prefix}/college-ff-{timestamp}.dump"

    with tempfile.TemporaryDirectory(prefix="cff-backup-") as temp_dir:
        dump_path = pathlib.Path(temp_dir) / "college-ff.dump"
        subprocess.run(
            [
                "pg_dump",
                db_url,
                "--format=custom",
                "--compress=9",
                "--no-owner",
                "--no-acl",
                f"--file={dump_path}",
            ],
            check=True,
            env={**os.environ, "PGCONNECT_TIMEOUT": "30"},
        )
        digest = hashlib.sha256(dump_path.read_bytes()).hexdigest()
        extra = {
            "Metadata": {"sha256": digest, "created-utc": now.isoformat()},
            "ContentType": "application/octet-stream",
        }
        if sse:
            extra["ServerSideEncryption"] = sse
        if sse == "aws:kms" and kms_key:
            extra["SSEKMSKeyId"] = kms_key
        client.upload_file(str(dump_path), bucket, key, ExtraArgs=extra)
        client.put_object(
            Bucket=bucket,
            Key=f"{key}.sha256",
            Body=f"{digest}  {pathlib.Path(key).name}\n".encode("utf-8"),
            ContentType="text/plain",
            **({"ServerSideEncryption": sse} if sse else {}),
            **({"SSEKMSKeyId": kms_key} if sse == "aws:kms" and kms_key else {}),
        )

    cutoff = now - dt.timedelta(days=retention_days)
    deleted = []
    paginator = client.get_paginator("list_objects_v2")
    for page in paginator.paginate(Bucket=bucket, Prefix=f"{prefix}/"):
        for item in page.get("Contents", []):
            object_key = item["Key"]
            if item["LastModified"] < cutoff and (
                object_key.endswith(".dump") or object_key.endswith(".dump.sha256")
            ):
                deleted.append({"Key": object_key})
                if len(deleted) == 1000:
                    client.delete_objects(Bucket=bucket, Delete={"Objects": deleted, "Quiet": True})
                    deleted.clear()
    if deleted:
        client.delete_objects(Bucket=bucket, Delete={"Objects": deleted, "Quiet": True})

    print(json.dumps({
        "status": "ok",
        "bucket": bucket,
        "key": key,
        "sha256": digest,
        "retentionDays": retention_days,
        "serverSideEncryption": sse,
    }, indent=2))


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(json.dumps({"status": "failed", "error": str(exc)}, indent=2), file=sys.stderr)
        raise SystemExit(1)
