#!/usr/bin/env python3
"""Validate the public health endpoint's transactional-email readiness signal."""

from __future__ import annotations

import json
import os
import sys
import urllib.request


BASE_URL = os.environ.get(
    "CFF_API_BASE_URL", "https://api.college-fantasy-football.com"
).rstrip("/")


def main() -> int:
    request = urllib.request.Request(
        f"{BASE_URL}/health",
        headers={"Accept": "application/json"},
    )
    with urllib.request.urlopen(request, timeout=30) as response:
        payload = json.load(response)

    configured = payload.get("emailDeliveryConfigured") is True
    result = {
        "status": "ok" if configured else "failed",
        "apiBaseUrl": BASE_URL,
        "emailDeliveryConfigured": payload.get("emailDeliveryConfigured"),
        "database": payload.get("database"),
    }
    print(json.dumps(result, indent=2, sort_keys=True))
    if not configured:
        print(
            "Email delivery is not configured according to /health. Check "
            "CFF_EMAIL_PROVIDER, RESEND_API_KEY, CFF_EMAIL_FROM, and "
            "CFF_FRONTEND_BASE_URL.",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
