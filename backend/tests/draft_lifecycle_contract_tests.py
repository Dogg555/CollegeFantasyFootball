#!/usr/bin/env python3
"""Source contracts for the authoritative multiplayer draft lifecycle."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HARDENING_PATHS = [
    "draft_lifecycle_hardening.cpp",
    "draft_lifecycle_hardening_db.inc",
    "draft_lifecycle_hardening_player.inc",
    "draft_lifecycle_hardening_auto.inc",
    "draft_lifecycle_hardening_payload.inc",
    "draft_lifecycle_hardening_controls.inc",
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
AUTO_MIGRATION = (ROOT / "backend" / "db" / "migrations" / "021_draft_room_auto_draft_reliability.sql").read_text(encoding="utf-8")
PHASE7_MIGRATION = (ROOT / "backend" / "db" / "migrations" / "027_draft_room_workflow.sql").read_text(encoding="utf-8")
CLIENT = (ROOT / "frontend" / "draft-lifecycle.js").read_text(encoding="utf-8")
PHASE7_CLIENT = (ROOT / "frontend" / "draft-phase7.js").read_text(encoding="utf-8")
STATE = (ROOT / "frontend" / "state.js").read_text(encoding="utf-8")
AUTH_BRIDGE = (ROOT / "frontend" / "draft-auth-bridge.js").read_text(encoding="utf-8")
DRAFT_HTML = (ROOT / "frontend" / "draft.html").read_text(encoding="utf-8")
INDEX_HTML = (ROOT / "frontend" / "index.html").read_text(encoding="utf-8")
LEAGUE_HTML = (ROOT / "frontend" / "league.html").read_text(encoding="utf-8")
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
require(AUTO_MIGRATION, "auto_draft_enabled", "participants need persisted auto-draft mode")
require(AUTO_MIGRATION, "consecutive_missed_picks", "participants need persisted missed-pick counts")
require(AUTO_MIGRATION, "selection_source", "picks need a source for manual versus auto selections")
require(AUTO_MIGRATION, "CREATE TABLE IF NOT EXISTS draft_activity_log", "draft activity log must be persistent")
require(PHASE7_MIGRATION, "paused_remaining_seconds", "pause/resume must preserve server clock state")
require(PHASE7_MIGRATION, "draft_timezone", "draft timezone must be persisted explicitly")
require(PHASE7_MIGRATION, "'cancelled'", "draft state constraint must support cancellation")

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
require(HARDENING, "resolveDueAutoDrafts", "server sync must resolve expired and auto-draft picks")
require(HARDENING, "selectAutoDraftCandidate", "server must choose auto-draft selections")
require(HARDENING, '"personal_queue"', "auto-draft must prefer manager queue entries")
require(HARDENING, '"system_ranking"', "auto-draft must fall back to system rankings")
require(HARDENING, "canonicalDraftPlayer", "manual and queued picks must reload the canonical player snapshot")
require(HARDENING, "COALESCE(p.active, TRUE) = TRUE", "draft candidates must come from the active player pool")
require(HARDENING, '"draft_player_ineligible"', "inactive and impossible-roster picks need a stable rejection code")
require(HARDENING, "kAutoDraftMissThreshold = 2", "missed picks must enable auto-draft after the configured threshold")
require(HARDENING, '"/draft/auto-draft"', "managers need an endpoint to enable or disable auto-draft")
require(HARDENING, '"/draft/settings"', "commissioners need an authoritative draft-settings endpoint")
require(HARDENING, '"/draft/pause"', "commissioners need an authoritative pause endpoint")
require(HARDENING, '"/draft/resume"', "commissioners need an authoritative resume endpoint")
require(HARDENING, '"/draft/cancel"', "commissioners need a pre-start cancellation endpoint")
require(HARDENING, 'status = \'paused\'', "pause must be persisted in draft state")
require(HARDENING, "paused_remaining_seconds", "resume must restore the saved server clock")
require(HARDENING, '"lifecycleState"', "draft snapshots need a normalized lifecycle state")
for state in ("NOT_SCHEDULED", "SCHEDULED", "LOBBY_OPEN", "IN_PROGRESS", "PAUSED", "COMPLETED", "CANCELLED"):
    require(HARDENING, state, f"normalized lifecycle must expose {state}")
require(HARDENING, '"recap"', "completed drafts must expose a recap")
require(HARDENING, '"draftFinalized"', "completed draft snapshots must identify finalized rosters")
require(HARDENING, "logDraftActivity", "draft lifecycle events must be logged")
require(HARDENING, "allManagersReady", "start must evaluate every active manager")
require(HARDENING, '"/draft/readiness"', "readiness endpoint must be handled by the lifecycle boundary")
require(HARDENING, "last_seen_at", "draft GET/readiness requests must maintain presence")
require(HARDENING, "draft_date <= NOW() + INTERVAL '30 minutes'", "draft lobby must auto-open before scheduled draft time")
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
require(CLIENT, "visibilitychange", "visible reconnects must re-fetch authoritative draft state")
require(CLIENT, "addEventListener?.('online'", "network recovery must re-fetch authoritative draft state")
require(CLIENT, "await syncDraft()", "conflicts must recover by fetching the latest board")
require(CLIENT, "draft-auto-toggle", "draft room must expose auto-draft controls")
require(PHASE7_CLIENT, "draft-pause", "draft room must expose commissioner pause control")
require(PHASE7_CLIENT, "draft-resume", "draft room must expose commissioner resume control")
require(PHASE7_CLIENT, "draft-save-settings", "draft room must expose persisted clock/timezone settings")
require(PHASE7_CLIENT, "draft-phase7-recap", "draft room must render the final recap")
require(PHASE7_CLIENT, "visibilitychange", "Phase 7 reconnect must refresh the authoritative snapshot")
require(PHASE7_CLIENT, "Idempotency-Key", "Phase 7 commissioner retries must remain idempotent")
require(STATE, "const DRAFT_LOBBY_AUTO_OPEN_MINUTES = 30", "frontend must mirror the server auto-open window")
require(STATE, "isTopOfHourDraftDate", "frontend must validate hourly draft times")
require(STATE, "combineDraftDateAndHour", "split draft date and hour controls must store one safe timestamp")
require(STATE, "populateDraftTimeSelect", "draft time dropdown must contain top-of-hour options")
require(DRAFT_HTML, '<script src="draft-lifecycle.js"></script>', "draft room must load readiness lifecycle explicitly")
require(DRAFT_HTML, '<script src="draft-phase7.js"></script>', "draft room must load Phase 7 controls")
require(DRAFT_HTML, 'id="draft-scheduled-time"', "draft room must show scheduled date/time near the top")
require(DRAFT_HTML, 'id="draft-activity-log"', "draft room must show a persistent activity log")
require(INDEX_HTML, 'id="draft-date" type="date" name="draftDate"', "create form draft date must be date-only")
require(INDEX_HTML, 'id="draft-time" name="draftTime"', "create form draft time must be a separate hourly dropdown")
require(LEAGUE_HTML, 'id="settings-draft-date" type="date" name="draftDate"', "settings draft date must be date-only")
require(LEAGUE_HTML, 'id="settings-draft-time" name="draftTime"', "settings draft time must be a separate hourly dropdown")
require(AUTH_BRIDGE, "installDraftRoomAccessGate();", "mobile draft access gate must install before full page load")

print("draft lifecycle source contracts passed")
