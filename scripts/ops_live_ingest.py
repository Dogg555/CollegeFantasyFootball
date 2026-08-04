#!/usr/bin/env python3
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


def decode_response(text):
    if not text:
        return {}
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        return {"raw": text}


def request(method, path, *, admin=False, timeout=60):
    headers = {
        "Accept": "application/json",
        "User-Agent": "college-ff-live-score-cron/2.0",
    }
    if admin:
        if not ADMIN_TOKEN:
            raise RuntimeError("CFF_ADMIN_API_TOKEN is required")
        headers["Authorization"] = f"Bearer {ADMIN_TOKEN}"
    req = urllib.request.Request(f"{BASE_URL}{path}", headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as response:
            status = response.getcode()
            body = response.read().decode("utf-8")
    except urllib.error.HTTPError as exc:
        status = exc.code
        body = exc.read().decode("utf-8")
    payload = decode_response(body)
    if status == 409 and payload.get("code") == "ingest_already_running":
        return payload
    if status < 200 or status >= 300:
        raise RuntimeError(f"{method} {path} failed with {status}: {payload}")
    return payload


def request_with_retries(path, retries, delay_seconds):
    last_error = None
    for attempt in range(1, retries + 1):
        try:
            return request("GET", path, timeout=20)
        except Exception as exc:
            last_error = exc
            if attempt == retries:
                break
            print(
                f"GET {path} attempt {attempt}/{retries} failed; retrying in {delay_seconds}s: {exc}",
                file=sys.stderr,
            )
            time.sleep(delay_seconds)
    raise RuntimeError(f"GET {path} failed after {retries} attempts: {last_error}")


def successful_worker_result(payload):
    status = str(payload.get("status", "")).lower()
    code = str(payload.get("code", "")).lower()
    if status == "succeeded":
        return True
    if status == "duplicate" and code == "duplicate_ingest":
        return True
    if status == "skipped" and code in {
        "duplicate_ingest",
        "ingest_already_running",
    }:
        return True
    return False


def main():
    if not BASE_URL.startswith(("http://", "https://")):
        raise RuntimeError("CFF_API_BASE_URL must start with http:// or https://")

    retries = positive_int_env("CFF_LIVE_INGEST_HEALTH_RETRIES", 3)
    retry_delay = positive_int_env("CFF_LIVE_INGEST_RETRY_DELAY_SECONDS", 5)
    timeout = positive_int_env("CFF_LIVE_INGEST_TIMEOUT_SECONDS", 90)

    result = {
        "health": request_with_retries("/health", retries, retry_delay),
        "apiHealth": request_with_retries("/api/health", retries, retry_delay),
    }
    result["ingest"] = request(
        "POST",
        "/api/admin/live-stats/run",
        admin=True,
        timeout=timeout,
    )
    if not successful_worker_result(result["ingest"]):
        raise RuntimeError(f"Live score ingestion did not complete successfully: {result['ingest']}")
    result["status"] = request(
        "GET",
        "/api/admin/live-stats/status",
        admin=True,
        timeout=30,
    )
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(json.dumps({"status": "failed", "error": str(exc)}, indent=2), file=sys.stderr)
        raise SystemExit(1)
