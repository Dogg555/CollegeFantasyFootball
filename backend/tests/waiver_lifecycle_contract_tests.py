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
    mutations = text("backend/src/waiver_lifecycle_hardening_mutations.inc")
    advice = text("backend/src/waiver_lifecycle_hardening_advice.inc")
    migration = text("backend/db/migrations/015_waiver_lifecycle_reliability.sql")
    frontend = text("frontend/waiver-lifecycle.js")
    config = text("frontend/config.js")
    cmake = text("backend/CMakeLists.txt")

    require('"waiver:" + leagueId' in db, "waiver advisory lock is absent")
    require("lockRosterLeague(connection, leagueId)" in db, "roster lock is not coordinated")
    require("waiver_operations" in db and "idempotentReplay" in db, "operation replay storage is absent")
    require("waiver_states" in db and "version = waiver_states.version + 1" in db,
            "monotonic waiver version is absent")
    require("ROW_NUMBER() OVER (ORDER BY priority" in db, "dense priority rotation is absent")

    require("SAVEPOINT waiver_claim_step" in mutations, "per-claim savepoint is absent")
    require("ROLLBACK TO SAVEPOINT waiver_claim_step" in mutations,
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

    require("requestMutation('process'" in frontend,
            "browser does not use replay-safe batch processing")
    require("expectedVersion: stateVersion()" in frontend,
            "browser mutations do not carry confirmed version")
    require("'Idempotency-Key': operation.operationKey" in frontend,
            "browser mutations do not carry operation key")
    require("await request();" in frontend and "if (!uncertainFailure(firstError))" in frontend,
            "uncertain retry boundary is absent")
    require("waiver-lifecycle.js" in config, "waiver lifecycle browser module is not loaded")
    require("src/waiver_lifecycle_hardening.cpp" in cmake,
            "production build does not include waiver lifecycle module")
    require("src/waiver_lifecycle.cpp" in cmake,
            "production build does not include waiver policy rules")
    require("waiver_lifecycle_tests" in cmake,
            "focused waiver lifecycle C++ target is absent")
    require("waiver_lifecycle_hardening_advice.inc" in module,
            "waiver advice is not installed by the production module")

    print("waiver lifecycle source contracts passed")


if __name__ == "__main__":
    main()
