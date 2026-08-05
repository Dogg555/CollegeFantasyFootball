#!/usr/bin/env python3
"""Structural contracts for authoritative roster transactions."""
from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"roster transaction contract failed: {message}")


def text(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def main() -> None:
    boundary = text("backend/src/roster_transaction_hardening.cpp")
    database = text("backend/src/roster_transaction_hardening_db.inc")
    mutations = text("backend/src/roster_transaction_hardening_mutations.inc")
    advice = text("backend/src/roster_transaction_hardening_advice.inc")
    migration = text("backend/db/migrations/014_roster_transaction_reliability.sql")
    frontend = text("frontend/roster-transactions.js")
    config = text("frontend/config.js")
    cmake = text("backend/CMakeLists.txt")

    require('registerSyncAdvice(rosterTransactionAdvice)' in advice,
            "production advice is not installed")
    require('/roster/transactions' in advice and '/roster/state' in advice,
            "authoritative roster endpoints are missing")
    require('/roster/drop' in advice and 'LegacySlot' in advice,
            "legacy roster mutations are not protected")
    require('pg_advisory_xact_lock' in database and '"roster:" + leagueId' in database,
            "league-wide roster serialization is missing")
    require('roster_operations' in database and 'response_payload' in database,
            "idempotent response replay is missing")
    require('expectedVersionMatches' in mutations and 'roster_state_conflict' in mutations,
            "optimistic concurrency is missing")
    require('RosterAction::Swap' in mutations and 'BEGIN' in database and 'ROLLBACK' in database,
            "atomic add/drop swap support is missing")
    require('player_unavailable' in mutations and 'player_already_rostered' in mutations,
            "duplicate ownership conflicts are not explicit")
    require('waiver_claim_required' in mutations and 'claimEndpoint' in mutations,
            "waiver-mode direct-add blocking is missing")
    require("league_matchups matchup" in database and "matchup.status = 'final'" in database,
            "finalized matchups must lock roster mutations")
    require("scoring_week_states scoring" in database and "scoring.status = 'final'" in database,
            "finalized scoring weeks must lock roster mutations")
    require("to_regclass($1) IS NOT NULL" in database,
            "optional lock tables must be checked before querying them")
    require("lineup_week_states" in database and "status = 'finalized'" in database
            and "schedule_week_states" in database and "status IN ('locked', 'finalized')" in database,
            "lineup and schedule locks must block roster mutations")
    require("Roster changes are locked after a matchup is finalized." in mutations and "lineup_locked" in mutations,
            "all roster mutations must reject when roster changes are locked")
    require('uq_rosters_league_player' in migration,
            "one-player-per-league database constraint is missing")
    require('ROW_NUMBER() OVER' in migration and 'ownership_rank > 1' in migration,
            "legacy duplicate ownership is not reconciled before the constraint")
    require('roster_states' in migration and 'roster_operations' in migration,
            "roster revision tables are missing")
    require('Idempotency-Key' in frontend and 'expectedVersion' in frontend,
            "browser mutations do not send replay and revision preconditions")
    require('requestWithUncertainRetry' in frontend and 'same roster operation will not run twice' in frontend,
            "uncertain mutation recovery is missing")
    require("'roster-transactions.js'" in config,
            "roster transaction browser adapter is not loaded")
    require('src/roster_transaction.cpp' in cmake and 'src/roster_transaction_hardening.cpp' in cmake,
            "roster transaction sources are not in the production target")
    require('roster_transaction_tests' in cmake,
            "roster transaction C++ tests are not registered")
    require('location.reload' not in frontend,
            "roster recovery must not reload the page")
    require('#include "roster_transaction_hardening_db.inc"' in boundary,
            "modular database boundary include is missing")

    print("roster transaction source contracts passed")


if __name__ == "__main__":
    main()
