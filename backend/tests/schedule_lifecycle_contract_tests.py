from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


rules = read("backend/src/schedule_lifecycle.cpp")
hardening = read("backend/src/schedule_lifecycle_hardening.cpp")
db = read("backend/src/schedule_lifecycle_hardening_db.inc")
mutations = read("backend/src/schedule_lifecycle_hardening_mutations.inc")
advice = read("backend/src/schedule_lifecycle_hardening_advice.inc")
migration = read("backend/db/migrations/018_schedule_lineup_lock_reliability.sql")
frontend = read("frontend/schedule-lineup-lifecycle.js")
config = read("frontend/config.js")
cmake = read("backend/CMakeLists.txt")

assert "canonicalManagers" in rules
assert "stableMatchupId" in rules
assert "scheduleFingerprint" in rules
assert "buildDeterministicSeason" in rules
assert "std::set<std::string>" in rules

assert '"schedule:" + leagueId' in db
assert "pg_advisory_xact_lock" in db
assert "schedule_operations" in db
assert "league_week_controls" in db
assert "currentLineupSnapshot" in db
assert "refreshExpiredDeadlines" in db
assert "cff_current_lineup_locked" in db

assert "expectedVersion" in mutations
assert "Idempotency-Key" in hardening
assert "schedule_state_conflict" in mutations
assert "schedule_locked" in mutations
assert "lineup_unlock_forbidden" in mutations
assert "persistSchedule" in mutations
assert "alreadyLocked" in mutations
assert "unchanged" in mutations
assert "storeScheduleOperation" in mutations

assert "/schedule/state" in advice
assert "/schedule/transactions" in advice
assert "/matchups/generate-season" in advice
assert "/lineup/week/" in hardening
assert "MutationGuard" in advice
assert "registerSyncAdvice" in advice

assert "CREATE TABLE IF NOT EXISTS league_schedule_states" in migration
assert "CREATE TABLE IF NOT EXISTS league_week_controls" in migration
assert "CREATE TABLE IF NOT EXISTS schedule_operations" in migration
assert "trg_rosters_lineup_lock" in migration
assert "trg_scoring_week_lineup_control" in migration
assert "cff_capture_lineup_snapshot" in migration
assert "current_week = GREATEST(current_week, NEW.week + 1)" in migration

assert "schedule-lineup-lifecycle.js" in config
assert config.index("schedule-lineup-lifecycle.js") < config.index("scoring-lifecycle.js")
assert "operationFor" in frontend
assert "uncertainFailure" in frontend
assert "schedule_state_conflict" in frontend
assert "lockLineupWeekApi" in frontend
assert "unlockLineupWeekApi" in frontend
assert "setLineupDeadlineApi" in frontend
assert "storage" in frontend
assert "visibilitychange" in frontend
assert "location.reload" not in frontend

assert "src/schedule_lifecycle.cpp" in cmake
assert "src/schedule_lifecycle_hardening.cpp" in cmake
assert "schedule_lifecycle_tests" in cmake

print("schedule lifecycle source contracts passed")
