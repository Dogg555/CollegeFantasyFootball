from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


rules = read("backend/src/stat_ingestion_lifecycle.cpp")
hardening = read("backend/src/stat_ingestion_hardening.cpp")
db = read("backend/src/stat_ingestion_hardening_db.inc")
payload = read("backend/src/stat_ingestion_hardening_payload.inc")
mutations = read("backend/src/stat_ingestion_hardening_mutations.inc")
advice = read("backend/src/stat_ingestion_hardening_advice.inc")
migration = read("backend/db/migrations/019_stat_ingestion_reliability.sql")
cmake = read("backend/CMakeLists.txt")

assert "statRecordKey" in rules
assert "statSourceHash" in rules
assert "retryDelaySeconds" in rules
assert "retryableProviderFailure" in rules
assert "recalculationStatus" in rules
assert "blocked_final" in rules

assert '"stat-ingest:"' in db
assert "pg_advisory_xact_lock" in db
assert "abandonExpiredRun" in db
assert "retryWindowActive" in db
assert "stat_ingestion_operations" in db
assert "sourceRevision" in payload
assert "recalculationQueue" in payload
assert "fresh" in payload

assert "expectedVersion" in mutations
assert "ingestion_run_active" in mutations
assert "ingestion_backoff_active" in mutations
assert "ingestion_lease_lost" in mutations
assert "duplicateRecords" in mutations
assert "player_stat_revisions" in mutations
assert "scoring_recalculation_queue" in mutations
assert "final_week_immutable" in rules
assert "source_hash" in mutations
assert "source_revision" in mutations
assert "recover" in mutations

assert "/api/admin/ingest/cfbd/stats/status" in advice
assert "/api/admin/ingest/cfbd/stats/transactions" in advice
assert "isAdminRequest" in advice
assert "registerSyncAdvice" in advice

assert "CREATE TABLE IF NOT EXISTS stat_ingestion_states" in migration
assert "CREATE TABLE IF NOT EXISTS stat_ingestion_operations" in migration
assert "CREATE TABLE IF NOT EXISTS player_stat_revisions" in migration
assert "CREATE TABLE IF NOT EXISTS scoring_recalculation_queue" in migration
assert "source_hash" in migration
assert "lease_expires_at" in migration
assert "next_retry_at" in migration
assert "cff_mark_stat_source_stale" in migration

assert "src/stat_ingestion_lifecycle.cpp" in cmake
assert "src/stat_ingestion_hardening.cpp" in cmake
assert "stat_ingestion_lifecycle_tests" in cmake

print("stat ingestion source contracts passed")
