#!/usr/bin/env python3
"""Source contracts for the authoritative multiplayer draft lifecycle."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HARDENING_PATHS = [
    "draft_lifecycle_hardening.cpp",
    "draft_lifecycle_hardening_db.inc",
    "draft_lifecycle_hardening_payload.inc",
    "draft_lifecycle_hardening_commissioner.inc",
    "draft_lifecycle_hardening_pick.inc",
    "draft_lifecycle_hardening_recovery.inc",
    "draft_lifecycle_hardening_advice.inc",
]
HARDENING = "\n".join(
    (ROOT / "backend" / "src" / path).read_text(encoding="utf-8")
    for path in HARDENING_PATHS
)
LIFECYCLE = (ROOT / "backend" / "src" / "draft_lifecycle.cpp").read_text(encoding="utf-8")
CPP_TESTS = (ROOT / "backend" / "tests" / "draft_lifecycle_tests.cpp").read_text(encoding="utf-8")
MIGRATION = (ROOT / "backend" / "db" / "migrations" / "013_draft_lifecycle_reliability.sql").read_text(encoding="utf-8")
CLIENT = (ROOT / "frontend" / "draft-lifecycle.js").read_text(encoding="utf-8")
CONFIG = (ROOT / "frontend" / "config.js").read_text(encoding="utf-8")
CMAKE = (ROOT / "backend" / "CMakeLists.txt").read_text(encoding="utf-8")


def require(source: str, fragment: str, message: str) -> None:
    if fragment not in source:
        raise AssertionError(message)


def script_index(name: str) -> int:
    marker = f"'{name}'"
    index = CONFIG.find(marker)
    if index < 0:
        raise AssertionError(f"missing shared script: {name}")
    return index


require(CMAKE, "src/draft_lifecycle.cpp", "production target must compile lifecycle rules")
require(CMAKE, "src/draft_lifecycle_hardening.cpp", "production target must compile draft transaction boundary")
require(CMAKE, "draft_lifecycle_tests", "core test target must exercise draft lifecycle rules")

require(MIGRATION, "ADD COLUMN IF NOT EXISTS version BIGINT", "draft snapshots need a monotonic version")
require(MIGRATION, "ADD COLUMN IF NOT EXISTS completed_at TIMESTAMPTZ", "completed drafts need a persisted timestamp")
require(MIGRATION, "CREATE TABLE IF NOT EXISTS draft_readiness", "manager readiness must be persisted")
require(MIGRATION, "CREATE TABLE IF NOT EXISTS draft_operations", "draft mutations need replay protection")
require(MIGRATION, "PRIMARY KEY (league_id, operation_key)", "operation keys must be unique within a league")

require(HARDENING, '"draft:" + leagueId', "all draft mutations must serialize per league")
require(HARDENING, "pg_advisory_xact_lock", "database advisory locking must guard simultaneous picks")
require(HARDENING, '"expectedVersion"', "mutations must carry an expected draft revision")
require(HARDENING, '"expectedPick"', "picks must carry the expected pick number")
require(HARDENING, '"draft_precondition_required"', "missing optimistic-concurrency inputs must fail closed")
require(HARDENING, '"draft_state_conflict"', "stale simultaneous actions need a stable conflict code")
require(HARDENING, "managerForPick(order, currentPick, draftType)", "server must determine the manager on the clock")
require(HARDENING, "preferredRosterSlot(player, rules, counts)", "server must place picks into valid roster slots")
require(HARDENING, "INSERT INTO draft_picks", "accepted picks must be persisted")
require(HARDENING, "INSERT INTO rosters", "accepted picks and roster state must commit together")
require(HARDENING, "draftCompleteAfterPick", "draft completion must use league size and roster rules")
require(HARDENING, "operationReplay", "commissioner and pick retries must replay safely")
require(HARDENING, "recordOperation", "confirmed draft operations must be recorded")
require(HARDENING, "allManagersReady", "start must evaluate every active manager")
require(HARDENING, '"/draft/readiness"', "readiness endpoint must be handled by the lifecycle boundary")
require(HARDENING, "last_seen_at", "draft GET/readiness requests must maintain presence")
require(HARDENING, "version = version + 1", "every authoritative draft mutation must advance revision")
require(HARDENING, "registerSyncAdvice(draftLifecycleAdvice)", "hardening must run before legacy draft handlers")

require(LIFECYCLE, "std::sort(emails.begin(), emails.end())", "default order must be deterministic")
require(LIFECYCLE, "league_schedule::currentDraftManager", "snake turns must use the shared deterministic schedule helper")
require(CPP_TESTS, "totalDraftPicks(4, rules) == 56", "4-team draft completion must be covered")
require(CPP_TESTS, "totalDraftPicks(6, rules) == 84", "6-team draft completion must be covered")
require(CPP_TESTS, "picksByManager", "full draft simulations must distribute every roster slot")

if not script_index("draft-poll-scope.js") < script_index("draft-lifecycle.js"):
    raise AssertionError("lifecycle client must load after poll scoping")
require(CLIENT, "shouldApplySnapshot", "reconnects must reject stale snapshots")
require(CLIENT, "'Idempotency-Key': operation.operationKey", "browser retries must reuse stable operation keys")
require(CLIENT, "expectedPick", "browser picks must send the confirmed pick number")
require(CLIENT, "expectedVersion", "browser mutations must send the confirmed revision")
require(CLIENT, "installRenderAdapter", "readiness gating must attach after the page renderer exists")
require(CLIENT, "draft-ready-toggle", "draft lobby must expose manager readiness controls")
require(CLIENT, "visibilitychange", "visible reconnects must re-fetch authoritative draft state")
require(CLIENT, "addEventListener?.('online'", "network recovery must re-fetch authoritative draft state")
require(CLIENT, "await syncDraft()", "conflicts must recover by fetching the latest board")

print("draft lifecycle source contracts passed")
