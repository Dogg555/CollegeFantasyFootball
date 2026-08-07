#!/usr/bin/env python3
"""Static contracts for the authoritative waiver lifecycle boundary."""
from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def text(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def main() -> None:
    module = text("backend/src/waiver_lifecycle_hardening.cpp")
    db = text("backend/src/waiver_lifecycle_hardening_db.inc")
    phase5_db = text("backend/src/waiver_lifecycle_hardening_phase5_db.inc")
    legacy_mutations = text("backend/src/waiver_lifecycle_hardening_mutations.inc")
    mutations = text("backend/src/waiver_lifecycle_hardening_phase5_mutations.inc")
    advice = text("backend/src/waiver_lifecycle_hardening_advice.inc")
    migration = text("backend/db/migrations/015_waiver_lifecycle_reliability.sql")
    phase5_migration = text("backend/db/migrations/026_waiver_processing_periods.sql")
    schema = text("backend/db/schema.sql")
    frontend = text("frontend/waiver-lifecycle.js")
    config = text("frontend/config.js")
    cmake = text("backend/CMakeLists.txt")
    workflow = text(".github/workflows/waiver-lifecycle-contracts.yml")

    require('"waiver:" + leagueId' in db, "waiver advisory lock is absent")
    require("lockRosterLeague(connection, leagueId)" in db, "roster lock is not coordinated")
    require("waiver_operations" in db and "idempotentReplay" in db, "operation replay storage is absent")
    require("waiver_states" in db and "version = waiver_states.version + 1" in db,
            "monotonic waiver version is absent")
    require("ROW_NUMBER() OVER (ORDER BY priority" in db, "dense priority rotation is absent")

    combined_mutations = legacy_mutations + "\n" + mutations
    require("SAVEPOINT waiver_claim_step" in combined_mutations, "per-claim savepoint is absent")
    require("ROLLBACK TO SAVEPOINT waiver_claim_step" in combined_mutations,
            "failed claim rollback is absent")
    require("waiver_claim_out_of_order" in mutations,
            "single-claim processing can bypass deterministic order")
    require("moveManagerToBackDb" in mutations,
            "winning manager priority advancement is absent")
    require("advanceRosterVersion" in mutations,
            "waiver processing does not advance roster revision")
    require("ON CONFLICT (league_id, player_id) DO NOTHING" in mutations,
            "one-owner database conflict handling is absent")
    require("commissioner_required" in mutations,
            "commissioner processing boundary is absent")
    require("validClaimReorder" in mutations,
            "exact claim reorder validation is absent")

    require("authoritativePlayerSnapshot" in mutations,
            "waiver awards do not reload the authoritative player record")
    require("freeAgentPlayerPoolEligible" in mutations,
            "waiver claims do not enforce the authoritative league player pool")
    require("freeAgentPlayerLocked" in mutations,
            "waiver processing does not revalidate the added player's game lock")
    require("slotMoveAllowed" in mutations and "drop_player_locked" in mutations,
            "conditional drops do not use the server lineup/game lock")
    require("addPlayerId" in mutations and "jsonToString(player)" in mutations,
            "claim creation is not based on an authoritative player ID")
    require("add_player_snapshot::text" not in mutations,
            "Phase 5 processing still consumes the stored client-era player snapshot")

    for action_label in (
        "The waiver deadline has passed for this processing period.",
        "Pending claims can no longer be cancelled after the waiver deadline.",
        "Pending claims can no longer be reordered after the waiver deadline.",
    ):
        require(action_label in mutations, f"pre-deadline claim management rule missing: {action_label}")

    require("effectiveWaiverProcessingPeriod" in phase5_db,
            "persisted processing-period resolution is absent")
    require("current_processing_period" in phase5_db and "current_processing_period" in phase5_migration,
            "active processing-period identity is not persisted independently of deadline edits")
    require("current == last" in phase5_db and "deadlinePassed(rules)" in phase5_db,
            "completed-period rollover is not tied to a newly opened claim window")
    require("effectiveWaiverProcessingPeriod(context->connection.get(), leagueId, rules)" in module,
            "Phase 5 mutations do not use the persisted processing-period resolver")
    require("last_processing_period" in phase5_db and "periodAlreadyProcessed" in mutations,
            "same-period semantic retry protection is absent")
    require("waiver_period_expired" in phase5_db and "status = 'expired'" in phase5_db,
            "older pending claims are not expired when the processing period advances")
    require("failure_reason" in phase5_db and "waiverFailureReason" in phase5_db,
            "readable persisted waiver failure reasons are absent")
    require('return "Successful"' in phase5_db and 'return "Expired"' in phase5_db,
            "Phase 5 claim statuses are not exposed to the UI")

    for route in (
        "/waivers/state",
        "/waivers/transactions",
        "/waivers/process",
        "/waivers/reorder",
        "/waiver-priority/reset",
    ):
        require(route in advice, f"route {route} is not protected by waiver advice")

    require("waiver_claims_status_check" in migration and "'failed'" in migration,
            "failed claim status schema is absent")
    require("uq_waiver_pending_manager_player" in migration,
            "duplicate pending claim constraint is absent")
    require("waiver_operations" in migration and "waiver_states" in migration,
            "waiver reliability tables are absent")
    require("processing_period" in phase5_migration and "failure_reason" in phase5_migration,
            "Phase 5 claim metadata migration is absent")
    require("'expired'" in phase5_migration and "last_processing_period" in phase5_migration,
            "Phase 5 expiry/period schema is absent")
    require("waiver_claims_status_check" in schema and "'failed'" in schema,
            "schema snapshot lacks baseline failed waiver claim status")
    require("CREATE TABLE IF NOT EXISTS waiver_states" in schema,
            "schema snapshot lacks waiver_states")
    require("CREATE TABLE IF NOT EXISTS waiver_operations" in schema,
            "schema snapshot lacks waiver_operations")

    require("requestMutation('process'" in frontend,
            "browser does not use replay-safe batch processing")
    require("expectedVersion: stateVersion()" in frontend,
            "browser mutations do not carry confirmed version")
    require("'Idempotency-Key': operation.operationKey" in frontend,
            "browser mutations do not carry operation key")
    require("await request();" in frontend and "if (!uncertainFailure(firstError))" in frontend,
            "uncertain retry boundary is absent")
    require("{ addPlayerId: playerId, dropPlayerId }" in frontend,
            "browser still sends a mutable player snapshot instead of the requested player ID")
    require("claimFailureMessage" in frontend and "failureReason" in frontend,
            "browser does not expose readable claim failure reasons")
    require("waiver_deadline_passed" in frontend and "drop_player_locked" in frontend,
            "browser is missing Phase 5 deadline/lock guidance")

    require("waiverPanelModel" in frontend and "claimsMutable" in frontend and "canProcess" in frontend,
            "browser waiver panel is not driven by authoritative claim-window/process state")
    require("button.disabled = !model.canProcess" in frontend,
            "commissioner process controls do not follow the authoritative canProcess flag")
    require("button.disabled = !model.claimsMutable" in frontend,
            "cancel controls do not close with the authoritative claim window")
    require("data-waiver-failure" in frontend and "claimFailureMessage(claim)" in frontend,
            "failed/expired claims do not render a readable reason")
    require("claimShowsFailureDetails" in frontend
            and "status === 'Failed'" in frontend
            and "status === 'Expired'" in frontend,
            "failure detail rendering is not limited to failed/expired claims")
    require("submitAuthoritativeWaiverForm" in frontend
            and "installWaiverSubmitOverlay" in frontend
            and "stopImmediatePropagation" in frontend,
            "server waiver submission can still fall through to the legacy finalized-matchup guard")
    require("addEventListener?.('submit'" in frontend and ", true);" in frontend,
            "authoritative waiver submit interception is not installed in capture phase")
    require("installRenderOverlay" in frontend and "applyWaiverPanelState" in frontend,
            "legacy league renders can overwrite the Phase 5 waiver panel state")
    require("lineupLocked" not in frontend,
            "Phase 5 waiver panel still relies on the historical whole-lineup lock")

    require("waiver_processing_period_runtime_contract.py" in workflow,
            "production CI does not exercise deadline edits without an explicit processingPeriod")
    require("026_waiver_processing_periods.sql" in workflow,
            "Phase 5 migration changes do not trigger the waiver lifecycle gate")
    require("waiver-lifecycle.js" in config, "waiver lifecycle browser module is not loaded")
    require("src/waiver_lifecycle_hardening.cpp" in cmake,
            "production build does not include waiver lifecycle module")
    require("src/waiver_lifecycle.cpp" in cmake,
            "production build does not include waiver policy rules")
    require("waiver_lifecycle_tests" in cmake,
            "focused waiver lifecycle C++ target is absent")
    require("waiver_lifecycle_hardening_phase5_db.inc" in module
            and "waiver_lifecycle_hardening_phase5_mutations.inc" in module,
            "production module does not install the Phase 5 hardening layer")
    require("waiver_lifecycle_hardening_advice.inc" in module,
            "waiver advice is not installed by the production module")

    print("waiver lifecycle source contracts passed")


if __name__ == "__main__":
    main()
