#!/usr/bin/env python3
"""Structural contracts for deterministic scoring and authoritative standings."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def require(path: str, *needles: str) -> None:
    text = (ROOT / path).read_text(encoding="utf-8")
    for needle in needles:
        assert needle in text, f"{path} is missing required contract: {needle}"


def main() -> None:
    require(
        "backend/src/scoring_lifecycle_hardening_advice.inc",
        'pathLeagueId(path, "/scoring/state")',
        'pathLeagueId(path, "/standings")',
        'pathLeagueId(path, "/scoring/transactions")',
        'parseScoreWeekPath(path, "/finalize"',
        "registerSyncAdvice(scoringLifecycleAdvice)",
    )
    require(
        "backend/src/scoring_lifecycle_hardening_db.inc",
        '"scoring:" + leagueId',
        "scoring_states",
        "scoring_week_states",
        "scoring_operations",
        "league_standings",
        "player_stats",
        "md5($1)",
    )
    require(
        "backend/src/scoring_lifecycle_hardening_mutations.inc",
        "expectedVersionMatches",
        "persistPlayerScores",
        "persistScoredMatchups",
        "persistScoredWeek",
        "persistFinalWeek",
        "standingsFromFinalMatchups",
        "replaceStandings",
        "week_finalized",
        "week_not_scored",
        "scoring_state_conflict",
        "alreadyFinal",
    )
    require(
        "backend/db/migrations/017_scoring_standings_reliability.sql",
        "CREATE TABLE IF NOT EXISTS scoring_states",
        "CREATE TABLE IF NOT EXISTS scoring_week_states",
        "CREATE TABLE IF NOT EXISTS scoring_operations",
        "CREATE TABLE IF NOT EXISTS league_standings",
        "scoring_settings_snapshot",
        "matchup_snapshot",
        "standings_version",
    )
    require(
        "frontend/scoring-lifecycle.js",
        "/scoring/state",
        "/scoring/transactions",
        "Idempotency-Key",
        "expectedVersion",
        "cff:scoring-lifecycle",
        "scoreWeekApi",
        "finalizeWeekApi",
        "standingsFromMatchups",
    )
    require("frontend/config.js", "scoring-lifecycle.js")
    require(
        "backend/CMakeLists.txt",
        "src/scoring_lifecycle.cpp",
        "src/scoring_lifecycle_hardening.cpp",
        "scoring_lifecycle_tests",
    )
    print("scoring lifecycle source contracts passed")


if __name__ == "__main__":
    main()
