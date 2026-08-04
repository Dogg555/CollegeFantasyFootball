from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def require(source: str, *needles: str) -> None:
    for needle in needles:
        assert needle in source, f"missing contract: {needle}"


def main() -> None:
    migration = read("backend/db/migrations/019_schedule_lineup_locking_reliability.sql")
    hardening = read("backend/src/schedule_lineup_hardening.cpp")
    db_helpers = read("backend/src/schedule_lineup_hardening_db.inc")
    mutations = read("backend/src/schedule_lineup_hardening_mutations.inc")
    advice = read("backend/src/schedule_lineup_hardening_advice.inc")
    scoring_advice = read("backend/src/scoring_lifecycle_hardening_advice.inc")
    browser = read("frontend/schedule-lineup-lifecycle.js")
    runtime = read("scripts/schedule_lineup_runtime_contract.py")
    config = read("frontend/config.js")
    cmake = read("backend/CMakeLists.txt")

    require(
        migration,
        "lineup_week_states_legacy_018",
        "column_name = 'manager_email'",
        "CREATE TABLE IF NOT EXISTS schedule_states",
        "CREATE TABLE IF NOT EXISTS schedule_operations",
        "CREATE TABLE IF NOT EXISTS schedule_week_states",
        "CREATE TABLE IF NOT EXISTS lineup_week_states",
        "CREATE TRIGGER trg_cff_enforce_locked_lineup_roster",
        "RAISE EXCEPTION 'lineup_locked'",
        "CREATE TRIGGER trg_cff_finalize_weekly_lineups",
        "schedule_version BIGINT",
    )
    require(
        hardening,
        'schedule_lineup_hardening_db.inc',
        'schedule_lineup_hardening_mutations.inc',
        'schedule_lineup_hardening_advice.inc',
        'prepareLineupsForScoringInternal',
        'const std::string marker = "/lineups/week/"',
    )
    require(
        db_helpers,
        'pg_advisory_xact_lock',
        '"schedule:" + leagueId',
        'managerHasActiveLineupLock',
        'playerIsLockedStarter',
        'persistSchedule',
        'schedule_input_hash',
    )
    require(
        mutations,
        'schedule_state_conflict',
        'schedule_locked',
        'lineup_invalid',
        'lineup_deadline_passed',
        'scoring_lock_permanent',
        'lockManagers',
        'unlockManagers',
        'prepareLineupsForScoringInternal',
        'storeOperation',
    )
    require(
        advice,
        '/schedule/state',
        '/schedule/transactions',
        '/matchups/generate-season',
        'parseLineupPath',
        'RosterSlotGuard',
        'RosterDropGuard',
        'registerSyncAdvice',
    )
    require(
        scoring_advice,
        'prepareLineupsForScoring',
        'action == "score" || action == "finalize"',
        'LegacyScore',
        'LegacyFinalize',
    )
    require(
        browser,
        'Idempotency-Key',
        'expectedVersion',
        'currentState',
        'BroadcastChannel',
        'visibilitychange',
        'lineup_locked',
        'lockWeeklyLineupApi',
        'setLineupDeadlineApi',
    )
    require(
        runtime,
        '"/api/auth/status"',
        '"/api/auth/signup"',
        '"/api/leagues"',
        'token.startswith("token-")',
        'CFF_CONTRACT_PASSWORD',
    )
    assert "INSERT INTO auth_tokens" not in runtime, "runtime must use production signed sessions"
    require(config, "'schedule-lineup-lifecycle.js'")
    require(
        cmake,
        'src/schedule_lineup_lifecycle.cpp',
        'src/schedule_lineup_hardening.cpp',
        'schedule_lineup_lifecycle_tests',
    )

    print("schedule lineup source contracts passed")


if __name__ == "__main__":
    main()