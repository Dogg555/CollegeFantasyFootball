#!/usr/bin/env python3
"""Structural contracts for weekly lineup rendering and game-start locks."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def require(source: str, *needles: str) -> None:
    for needle in needles:
        assert needle in source, f"missing lineup management contract: {needle}"


def main() -> None:
    lock_header = read("backend/src/lineup_game_lock.h")
    roster_boundary = read("backend/src/roster_transaction_hardening.cpp")
    roster_policy = read("backend/src/league_roster.cpp")
    composition = read("backend/src/app_composition.cpp")
    browser = read("frontend/lineup-management.js")
    config = read("frontend/config.js")

    require(
        lock_header,
        '"/api/leagues/{1}/lineup-locks"',
        "scheduled.start_date <= NOW()",
        "scheduled.season = $3::int",
        "scheduled.week = $4::int",
        "lineup_week_states",
        "schedule_week_states",
        "slotMoveAllowed",
        "validateRosterSlotMoveWithWeeklyLock",
    )
    require(
        roster_boundary,
        '#include "lineup_game_lock.h"',
        "validateRosterSlotMoveWithWeeklyLock",
        "action == RosterAction::Slot",
    )
    require(
        composition,
        '#include "lineup_game_lock.h"',
        "cff::lineup_game_lock::registerRoutes(app);",
    )
    require(
        browser,
        "Starting lineup",
        "Bench",
        "Empty starter slot",
        "score 0 points",
        "Move to ${slot.toUpperCase()}",
        "playerLocked",
        "CFFRosterTransactions?.mutate",
        "cff:roster-transaction",
        "/lineup-locks",
    )
    require(config, "'lineup-management.js'", "'lineup-management.css'")
    require(roster_policy, "Empty starter slots are legal")
    assert '"Missing " + std::to_string(required - filled)' not in roster_policy

    print("weekly lineup management source contracts passed")


if __name__ == "__main__":
    main()
