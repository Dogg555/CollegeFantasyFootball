#!/usr/bin/env python3
"""Structural contract for the durable live-stat worker integration."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def require(path: str, *tokens: str) -> str:
    text = (ROOT / path).read_text(encoding="utf-8")
    missing = [token for token in tokens if token not in text]
    if missing:
        raise AssertionError(f"{path} is missing required contract tokens: {missing}")
    return text


def main() -> None:
    worker = require(
        "backend/src/live_stat_worker.cpp",
        "stat_ingest_runs",
        "stat_ingest_source_results",
        "stat_source_freshness",
        "ingest_operator_events",
        "CFF_LIVE_STAT_MAX_ATTEMPTS",
        "CFF_LIVE_STAT_RETRY_BASE_MS",
        "mayStartRun",
        "aggregateStatus",
        "scoringRefreshReady",
    )
    require(
        "backend/src/live_stat_routes.cpp",
        "/api/admin/live-stats/run",
        "/api/admin/live-stats/status",
        "requireAdmin",
        "runCfbdLiveStatWorker",
        "liveStatOperatorStatus",
    )
    require(
        "backend/src/application_bootstrap.cpp",
        "configureLiveStatWorker",
    )
    require(
        "backend/src/app_composition.cpp",
        "registerLiveStatRoutes",
    )
    require(
        "backend/CMakeLists.txt",
        "src/live_stat_orchestration.cpp",
        "src/live_stat_worker.cpp",
        "src/live_stat_routes.cpp",
    )
    env = require(
        ".env.example",
        "CFF_LIVE_STAT_ON_STARTUP",
        "CFF_LIVE_STAT_INTERVAL_MINUTES",
        "CFF_LIVE_STAT_MAX_ATTEMPTS",
        "CFF_LIVE_STAT_DEDUPE_MINUTES",
    )

    # Score refresh must not be queued from scoreboard-only data. That remains
    # gated until the player-stat adapter supplies authoritative stat changes.
    if "INSERT INTO scoring_refresh_queue" in worker:
        raise AssertionError(
            "scoreboard-only worker must not enqueue fantasy scoring refreshes"
        )
    if "CFF_LIVE_STAT_INTERVAL_MINUTES" not in env:
        raise AssertionError("scheduled worker configuration is undocumented")

    print("live stat worker integration contract passed")


if __name__ == "__main__":
    main()
