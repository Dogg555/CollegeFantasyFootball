#!/usr/bin/env python3
"""Evaluate deployed College Fantasy Football infrastructure against Alpha gates."""
from __future__ import annotations

import json
import os
import re
import sys
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

API_BASE = os.environ.get("CFF_API_BASE_URL", "").rstrip("/")
ADMIN_TOKEN = os.environ.get("CFF_ADMIN_API_TOKEN", "")
REQUIRE_EMAIL = os.environ.get("CFF_ALPHA_REQUIRE_EMAIL", "false").lower() == "true"
BACKUP_EVIDENCE_SHA256 = os.environ.get("CFF_ALPHA_BACKUP_EVIDENCE_SHA256", "").strip().lower()
MIN_PLAYERS = int(os.environ.get("CFF_ALPHA_MIN_ACTIVE_PLAYERS", "1000"))
MAX_MONTHLY_CALLS = int(os.environ.get("CFF_ALPHA_MAX_MONTHLY_CFBD_CALLS", "125000"))
OUT_DIR = Path(os.environ.get("CFF_ALPHA_REPORT_DIR", "alpha-readiness-artifacts"))
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")


def request(path: str, *, admin: bool = False) -> tuple[int, Any]:
    headers = {"Accept": "application/json"}
    if admin and ADMIN_TOKEN:
        headers["Authorization"] = f"Bearer {ADMIN_TOKEN}"
    req = urllib.request.Request(f"{API_BASE}{path}", headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=45) as response:
            raw = response.read().decode("utf-8")
            return response.status, json.loads(raw) if raw else None
    except urllib.error.HTTPError as exc:
        raw = exc.read().decode("utf-8", errors="replace")
        try:
            payload = json.loads(raw)
        except json.JSONDecodeError:
            payload = raw
        return exc.code, payload
    except Exception as exc:
        return 0, {"error": str(exc)}


def add(results: list[dict[str, Any]], gate: str, passed: bool, detail: str, required: bool = True) -> None:
    results.append({"gate": gate, "passed": passed, "required": required, "detail": detail})


def main() -> int:
    if not API_BASE:
        print("CFF_API_BASE_URL is required", file=sys.stderr)
        return 2

    results: list[dict[str, Any]] = []
    status, health = request("/health")
    health = health if isinstance(health, dict) else {}
    add(results, "API health", status == 200 and health.get("status") == "ok", f"HTTP {status}; status={health.get('status')}")
    add(results, "Database health", health.get("database") == "ok", f"database={health.get('database')}")
    add(results, "CORS configured", health.get("allowedOriginsConfigured") is True, f"allowedOriginsConfigured={health.get('allowedOriginsConfigured')}")
    add(results, "JWT configured", health.get("jwtSecretConfigured") is True, f"jwtSecretConfigured={health.get('jwtSecretConfigured')}")
    add(results, "Email delivery configured", health.get("emailDeliveryConfigured") is True, f"emailDeliveryConfigured={health.get('emailDeliveryConfigured')}", REQUIRE_EMAIL)

    player_query = urllib.parse.urlencode({"query": "%", "limit": "1"})
    status, players = request(f"/api/players?{player_query}")
    player_browse_ok = status == 200 and isinstance(players, list) and len(players) > 0
    add(results, "Player browse", player_browse_ok, f"HTTP {status}; returned={len(players) if isinstance(players, list) else 'invalid'}")

    status, ingest = request("/api/admin/ingest/cfbd/status", admin=True)
    ingest = ingest if isinstance(ingest, dict) else {}
    counts = ingest.get("counts") if isinstance(ingest.get("counts"), dict) else {}
    active_players = int(counts.get("activePlayers", counts.get("players", 0)) or 0)
    add(results, "Player ingestion status", status == 200 and ingest.get("status") == "ok", f"HTTP {status}; status={ingest.get('status')}")
    add(results, "Current player population", active_players >= MIN_PLAYERS, f"active/current players={active_players}; minimum={MIN_PLAYERS}")

    status, scores = request("/api/scores/live")
    games = scores if isinstance(scores, list) else []
    add(results, "Schedule cache endpoint", status == 200, f"HTTP {status}; cached games={len(games)}")
    add(results, "Schedule cache populated", len(games) > 0, f"cached games={len(games)}")

    status, live = request("/api/admin/ingest/cfbd/live/status", admin=True)
    live = live if isinstance(live, dict) else {}
    monthly_calls = int(live.get("monthlyApiCalls", 0) or 0)
    add(results, "Live ingestion status", status == 200 and live.get("status") in {"ok", "idle"}, f"HTTP {status}; status={live.get('status')}")
    add(results, "CFBD quota", monthly_calls < MAX_MONTHLY_CALLS, f"monthly calls={monthly_calls}; allowance={MAX_MONTHLY_CALLS}")

    backup_valid = bool(SHA256_RE.fullmatch(BACKUP_EVIDENCE_SHA256))
    backup_detail = (
        f"restore evidence sha256={BACKUP_EVIDENCE_SHA256[:12]}…"
        if backup_valid
        else "CFF_ALPHA_BACKUP_EVIDENCE_SHA256 must contain the digest emitted by a successful restore_backup.py run"
    )
    add(results, "Off-platform backup restore evidence", backup_valid, backup_detail)

    required_failures = [result for result in results if result["required"] and not result["passed"]]
    classification = "alpha-candidate" if not required_failures else "pre-alpha"
    report = {
        "generatedAt": datetime.now(timezone.utc).isoformat(),
        "apiBaseUrl": API_BASE,
        "classification": classification,
        "requiredFailures": len(required_failures),
        "backupRestoreEvidenceSha256": BACKUP_EVIDENCE_SHA256 if backup_valid else None,
        "results": results,
    }

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    (OUT_DIR / "alpha-readiness.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    lines = ["# Alpha readiness report", "", f"**Classification:** `{classification}`", "", "| Gate | Result | Detail |", "|---|---|---|"]
    for result in results:
        mark = "PASS" if result["passed"] else ("FAIL" if result["required"] else "WARN")
        detail = str(result["detail"]).replace("|", "\\|")
        lines.append(f"| {result['gate']} | {mark} | {detail} |")
    (OUT_DIR / "alpha-readiness.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    return 1 if required_failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
