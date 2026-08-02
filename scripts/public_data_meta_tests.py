#!/usr/bin/env python3
import datetime as dt
import json
import os
from pathlib import Path
import urllib.parse
import urllib.request

BASE = os.environ.get("CFF_API_BASE_URL", "http://127.0.0.1:8080").rstrip("/")
HOST = (urllib.parse.urlparse(BASE).hostname or "").lower()
IS_LOCAL = HOST in {"127.0.0.1", "localhost", "::1"}


def int_env(name: str, default: int) -> int:
    value = int(os.environ.get(name, str(default)))
    if value < 0:
        raise ValueError(f"{name} must be zero or greater")
    return value


def bool_env(name: str, default: bool) -> bool:
    raw = os.environ.get(name)
    if raw is None:
        return default
    return raw.strip().lower() in {"1", "true", "yes", "on"}


MIN_ACTIVE_PLAYERS = int_env("CFF_PUBLIC_MIN_ACTIVE_PLAYERS", 0 if IS_LOCAL else 1000)
MIN_PLAYER_SAMPLE = int_env("CFF_PUBLIC_MIN_PLAYER_SAMPLE", 0 if IS_LOCAL else 1)
REQUIRE_SCHEDULE = bool_env("CFF_PUBLIC_REQUIRE_SCHEDULE", not IS_LOCAL)
MIN_GAMES = int_env("CFF_PUBLIC_MIN_GAMES", 1 if REQUIRE_SCHEDULE else 0)
REPORT_DIR = Path(os.environ.get("CFF_RELEASE_GATE_REPORT_DIR", "release-gate-artifacts"))


def get(path: str):
    request = urllib.request.Request(BASE + path, headers={"Accept": "application/json"})
    with urllib.request.urlopen(request, timeout=20) as response:
        if response.status != 200:
            raise AssertionError(f"{path} returned {response.status}")
        return json.load(response)


players_meta = get("/api/players/meta")
assert isinstance(players_meta, dict), players_meta
player_status = players_meta.get("status")
active_players = players_meta.get("activePlayers", 0)
assert isinstance(active_players, int), players_meta

if MIN_ACTIVE_PLAYERS > 0:
    assert player_status == "ok", players_meta
    assert active_players >= MIN_ACTIVE_PLAYERS, (
        f"activePlayers={active_players} is below required minimum "
        f"{MIN_ACTIVE_PLAYERS}"
    )
else:
    assert player_status in {"ok", "unavailable"}, players_meta

scores_meta = get("/api/scores/live/meta")
assert isinstance(scores_meta, dict), scores_meta
schedule_status = scores_meta.get("status")
game_count = scores_meta.get("gameCount", 0)
assert isinstance(game_count, int), scores_meta

if REQUIRE_SCHEDULE:
    assert schedule_status == "ok", scores_meta
    assert game_count >= MIN_GAMES, (
        f"gameCount={game_count} is below required minimum {MIN_GAMES}"
    )
else:
    assert schedule_status in {"ok", "never", "failed", "unavailable"}, scores_meta

players = get("/api/players?limit=5&offset=0")
assert isinstance(players, list), players
assert len(players) <= 5, players
if MIN_PLAYER_SAMPLE > 0:
    assert len(players) >= MIN_PLAYER_SAMPLE, (
        f"player sample contains {len(players)} row(s); "
        f"{MIN_PLAYER_SAMPLE} required"
    )

checked_at = dt.datetime.now(dt.timezone.utc).replace(microsecond=0).isoformat()
report = {
    "status": "passed",
    "checkedAtUtc": checked_at,
    "apiBase": BASE,
    "thresholds": {
        "minActivePlayers": MIN_ACTIVE_PLAYERS,
        "minPlayerSample": MIN_PLAYER_SAMPLE,
        "requireSchedule": REQUIRE_SCHEDULE,
        "minGames": MIN_GAMES,
    },
    "playersMeta": players_meta,
    "scoresMeta": scores_meta,
    "playerSampleCount": len(players),
}

markdown = "\n".join(
    [
        "# Public data metadata validation",
        "",
        "- Status: passed",
        f"- Checked: {checked_at}",
        f"- API: {BASE}",
        f"- Active players: {active_players} (minimum {MIN_ACTIVE_PLAYERS})",
        f"- Player sample rows: {len(players)} (minimum {MIN_PLAYER_SAMPLE})",
        f"- Schedule status: {schedule_status}",
        f"- Games: {game_count} (minimum {MIN_GAMES})",
        "",
    ]
)

REPORT_DIR.mkdir(parents=True, exist_ok=True)
(REPORT_DIR / "public-data-meta-validation.json").write_text(
    json.dumps(report, indent=2, sort_keys=True) + "\n",
    encoding="utf-8",
)
(REPORT_DIR / "public-data-meta-validation.md").write_text(markdown, encoding="utf-8")

step_summary = os.environ.get("GITHUB_STEP_SUMMARY")
if step_summary:
    with open(step_summary, "a", encoding="utf-8") as handle:
        handle.write("\n" + markdown)

print(
    "public player and schedule metadata validation passed: "
    f"activePlayers={active_players}, gameCount={game_count}, "
    f"sample={len(players)}"
)
