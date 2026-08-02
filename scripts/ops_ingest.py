#!/usr/bin/env python3
import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.request


BASE_URL = os.environ.get("CFF_API_BASE_URL", "http://127.0.0.1:8080").rstrip("/")
ADMIN_TOKEN = os.environ.get("CFF_ADMIN_API_TOKEN", "")


def positive_int_env(name, default):
    raw = os.environ.get(name, "").strip()
    if not raw:
        return default
    try:
        value = int(raw)
    except ValueError as exc:
        raise RuntimeError(f"{name} must be a positive integer") from exc
    if value <= 0:
        raise RuntimeError(f"{name} must be a positive integer")
    return value


def positive_float_env(name, default):
    raw = os.environ.get(name, "").strip()
    if not raw:
        return default
    try:
        value = float(raw)
    except ValueError as exc:
        raise RuntimeError(f"{name} must be a positive number") from exc
    if value <= 0:
        raise RuntimeError(f"{name} must be a positive number")
    return value


def decode_response(text):
    if not text:
        return {}
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        return {"raw": text}


def request(method, path, admin=False, timeout=120):
    headers = {
        "Accept": "application/json",
        "User-Agent": "college-ff-ingestion-cron/1.0",
    }
    if admin:
        if not ADMIN_TOKEN:
            raise RuntimeError("CFF_ADMIN_API_TOKEN is required")
        headers["Authorization"] = f"Bearer {ADMIN_TOKEN}"
    req = urllib.request.Request(
        f"{BASE_URL}{path}",
        headers=headers,
        method=method,
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            text = resp.read().decode("utf-8")
            status = resp.getcode()
    except urllib.error.HTTPError as exc:
        text = exc.read().decode("utf-8")
        status = exc.code
    payload = decode_response(text)
    if status < 200 or status >= 300:
        raise RuntimeError(f"{method} {path} failed with {status}: {payload}")
    return payload


def request_with_retries(method, path, *, admin=False, timeout=120, retries=5, retry_delay=5.0):
    last_error = None
    for attempt in range(1, retries + 1):
        try:
            return request(method, path, admin=admin, timeout=timeout)
        except Exception as exc:
            last_error = exc
            if attempt == retries:
                break
            print(
                f"{method} {path} attempt {attempt}/{retries} failed; retrying in {retry_delay:g}s: {exc}",
                file=sys.stderr,
            )
            time.sleep(retry_delay)
    raise RuntimeError(f"{method} {path} failed after {retries} attempts: {last_error}")


def main():
    parser = argparse.ArgumentParser(description="Private CFBD ingestion operations helper.")
    parser.add_argument("--run", action="store_true", help="Trigger a one-off CFBD ingest before reading status.")
    parser.add_argument("--skip-health", action="store_true", help="Skip public health checks before admin calls.")
    parser.add_argument(
        "--allow-partial",
        action="store_true",
        help="Exit successfully when the ingestion endpoint reports a partial result.",
    )
    args = parser.parse_args()

    if not BASE_URL.startswith(("http://", "https://")):
        raise RuntimeError("CFF_API_BASE_URL must start with http:// or https://")

    ingest_timeout = positive_int_env("CFF_INGEST_TIMEOUT_SECONDS", 900)
    health_retries = positive_int_env("CFF_INGEST_HEALTH_RETRIES", 5)
    retry_delay = positive_float_env("CFF_INGEST_RETRY_DELAY_SECONDS", 5.0)

    result = {}
    if not args.skip_health:
        result["health"] = request_with_retries(
            "GET",
            "/health",
            timeout=20,
            retries=health_retries,
            retry_delay=retry_delay,
        )
        result["apiHealth"] = request_with_retries(
            "GET",
            "/api/health",
            timeout=20,
            retries=health_retries,
            retry_delay=retry_delay,
        )
    if args.run:
        result["ingest"] = request(
            "POST",
            "/api/admin/ingest/cfbd",
            admin=True,
            timeout=ingest_timeout,
        )
        ingest_status = str(result["ingest"].get("status", "")).lower()
        if ingest_status != "ok" and not args.allow_partial:
            raise RuntimeError(f"CFBD ingestion did not complete successfully: {result['ingest']}")
    result["status"] = request_with_retries(
        "GET",
        "/api/admin/ingest/cfbd/status",
        admin=True,
        timeout=30,
        retries=health_retries,
        retry_delay=retry_delay,
    )
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(json.dumps({"status": "failed", "error": str(exc)}, indent=2), file=sys.stderr)
        raise SystemExit(1)
