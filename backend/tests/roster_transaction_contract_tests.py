#!/usr/bin/env python3
"""Structural contracts for authoritative roster and immediate free-agent transactions."""
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
    directory = text("backend/src/roster_transaction_hardening_directory.inc")
    mutations = text("backend/src/roster_transaction_hardening_mutations.inc")
    advice = text("backend/src/roster_transaction_hardening_advice.inc")
    migration = text("backend/db/migrations/014_roster_transaction_reliability.sql")
    frontend = text("frontend/roster-transactions.js")
    player_page = text("frontend/players.js")
    player_model = text("frontend/free-agent-directory.js")
    player_html = text("frontend/players.html")
    config = text("frontend/config.js")
    cmake = text("backend/CMakeLists.txt")

    require('registerSyncAdvice(rosterTransactionAdvice)' in advice,
            "production advice is not installed")
    require('/roster/transactions' in advice and '/roster/state' in advice,
            "authoritative roster endpoints are missing")
    require('pathLeagueId(path, "/players")' in advice and 'getFreeAgentDirectory' in advice,
            "league-scoped player availability endpoint is missing")
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

    require('authoritativePlayerSnapshot' in directory and "FROM players WHERE id = $1 AND active = TRUE" in directory,
            "free-agent adds must resolve the active authoritative player record")
    require('authoritativePlayerSnapshot(connection.get(), addPlayerId)' in mutations,
            "add/swap does not use the authoritative player snapshot")
    require(mutations.index('authoritativePlayerSnapshot(connection.get(), addPlayerId)')
            < mutations.index('DELETE FROM rosters'),
            "player-pool validation must happen before any swap delete")
    require('freeAgentPlayerPoolEligible' in directory and 'player_ineligible' in mutations,
            "draftable-position eligibility is not enforced")
    require('freeAgentPlayerLocked' in directory and 'player_locked' in mutations,
            "started free-agent games are not revalidated")
    require('slotMoveAllowed' in mutations and 'drop_player_locked' in mutations,
            "drop-player lock state is not revalidated")
    require('if (!slotAction) return false;' in boundary,
            "historical finalized weeks must not globally freeze future free-agent activity")

    require('owner_email' in directory and 'pending_waiver' in directory and 'roster_percentage' in directory,
            "directory ownership, waiver, or roster-percentage state is missing")
    require('opponent' in directory and 'game_status' in directory and 'game_start_time' in directory,
            "directory opponent/game context is missing")
    require('capabilities["points"] = false' in directory
            and 'capabilities["projections"] = false' in directory,
            "unsupported points/projection fields must be disclosed instead of fabricated")
    require('"available", "rostered", "owned", "waivers", "locked", "ineligible"' in directory,
            "normalized availability vocabulary is missing")
    require('roster_rules AS (' in directory and "THEN 'ineligible'" in directory,
            "league position eligibility must be part of the paginated directory query")
    require('predicates.push_back("directory.availability = $"' in directory,
            "availability filtering must happen before LIMIT/OFFSET")
    require(directory.index('predicates.push_back("directory.availability = $"')
            < directory.index('sql += " LIMIT $"'),
            "availability filtering is applied after pagination")

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

    require('buildRosterPreview' in player_model and 'eligibleDropCandidates' in player_model,
            "browser roster-impact model is missing")
    require('addFreeAgentApi(previewPlayer, dropId)' in player_page,
            "player directory does not confirm through the atomic roster adapter")
    require('syncRosterForDirectory()' in player_page and 'fetchRosterLocks()' in player_page,
            "preview does not refresh roster and lock state")
    require('playerSearchQueued = true' in player_page
            and 'if (playerSearchQueued) return;' in player_page
            and 'void loadPlayerPool();' in player_page,
            "overlapping directory requests can discard the newest search")
    require('previewWeekLocked = lockState?.weekLocked === true' in player_page
            and 'previewConfirm.disabled = previewBusy || !preview.valid || (needsDrop && previewWeekLocked)' in player_page,
            "whole-week lock state is not enforced in full-roster add/drop previews")
    require('refreshLeagueDashboard' in player_page,
            "successful add/drop does not refresh league dashboard state")
    require('free-agent-preview' in player_html and 'free-agent-drop-select' in player_html,
            "add/drop confirmation interface is missing")
    require('points-filter' in player_html and 'disabled' in player_html,
            "unsupported points/projection filters must remain visibly disabled")

    require('src/roster_transaction.cpp' in cmake and 'src/roster_transaction_hardening.cpp' in cmake,
            "roster transaction sources are not in the production target")
    require('roster_transaction_tests' in cmake,
            "roster transaction C++ tests are not registered")
    require('location.reload' not in frontend and 'location.reload' not in player_page,
            "roster recovery must not reload the page")
    require('#include "roster_transaction_hardening_directory.inc"' in boundary,
            "free-agent directory module is not composed into production")

    print("roster transaction and free-agent source contracts passed")


if __name__ == "__main__":
    main()
