#!/usr/bin/env python3
import argparse
import json
import os
import sys
import urllib.error
import urllib.request


BASE_URL = os.environ.get("CFF_API_BASE_URL", "http://127.0.0.1:8080").rstrip("/")
ADMIN_TOKEN = os.environ.get("CFF_ADMIN_API_TOKEN", "")


def decode_response(text):
    if not text:
        return {}
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        return {"raw": text}


def request(method, path, admin=False, timeout=120):
    headers = {"Accept": "application/json"}
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


def main():
    parser = argparse.ArgumentParser(description="Private CFBD ingestion operations helper.")
    parser.add_argument("--run", action="store_true", help="Trigger a one-off CFBD ingest before reading status.")
    parser.add_argument("--skip-health", action="store_true", help="Skip public health checks before admin calls.")
    args = parser.parse_args()

    result = {}
    if not args.skip_health:
        result["health"] = request("GET", "/health", timeout=20)
        result["apiHealth"] = request("GET", "/api/health", timeout=20)
    if args.run:
        result["ingest"] = request("POST", "/api/admin/ingest/cfbd", admin=True)
    result["status"] = request("GET", "/api/admin/ingest/cfbd/status", admin=True)
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(json.dumps({"status": "failed", "error": str(exc)}, indent=2), file=sys.stderr)
        raise SystemExit(1)
