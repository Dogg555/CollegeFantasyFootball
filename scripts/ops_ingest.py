#!/usr/bin/env python3
import argparse
import json
import os
import sys
import urllib.error
import urllib.request


BASE_URL = os.environ.get("CFF_API_BASE_URL", "http://127.0.0.1:8080").rstrip("/")
ADMIN_TOKEN = os.environ.get("CFF_ADMIN_API_TOKEN", "")


def request(method, path):
    if not ADMIN_TOKEN:
        raise RuntimeError("CFF_ADMIN_API_TOKEN is required")
    req = urllib.request.Request(
        f"{BASE_URL}{path}",
        headers={
            "Accept": "application/json",
            "Authorization": f"Bearer {ADMIN_TOKEN}",
        },
        method=method,
    )
    try:
        with urllib.request.urlopen(req, timeout=120) as resp:
            text = resp.read().decode("utf-8")
            status = resp.getcode()
    except urllib.error.HTTPError as exc:
        text = exc.read().decode("utf-8")
        status = exc.code
    payload = json.loads(text) if text else {}
    if status < 200 or status >= 300:
        raise RuntimeError(f"{method} {path} failed with {status}: {payload}")
    return payload


def main():
    parser = argparse.ArgumentParser(description="Private CFBD ingestion operations helper.")
    parser.add_argument("--run", action="store_true", help="Trigger a one-off CFBD ingest before reading status.")
    args = parser.parse_args()

    result = {}
    if args.run:
        result["ingest"] = request("POST", "/api/admin/ingest/cfbd")
    result["status"] = request("GET", "/api/admin/ingest/cfbd/status")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(json.dumps({"status": "failed", "error": str(exc)}, indent=2), file=sys.stderr)
        raise SystemExit(1)
