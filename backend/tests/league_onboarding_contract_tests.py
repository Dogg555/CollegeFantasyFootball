#!/usr/bin/env python3
"""Source contracts for idempotent league creation, joining, and approval."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BACKEND = (ROOT / "backend" / "src" / "league_onboarding_hardening.cpp").read_text(encoding="utf-8")
SNAKE_BACKEND = (ROOT / "backend" / "src" / "snake_draft_only.cpp").read_text(encoding="utf-8")
CMAKE = (ROOT / "backend" / "CMakeLists.txt").read_text(encoding="utf-8")
MIGRATION = (ROOT / "backend" / "db" / "migrations" / "012_league_onboarding_idempotency.sql").read_text(encoding="utf-8")
FRONTEND = (ROOT / "frontend" / "league-onboarding.js").read_text(encoding="utf-8")
SNAKE_FRONTEND = (ROOT / "frontend" / "snake-draft-only.js").read_text(encoding="utf-8")
CONFIG = (ROOT / "frontend" / "config.js").read_text(encoding="utf-8")
ROUTES = (ROOT / "backend" / "src" / "league_routes.cpp").read_text(encoding="utf-8")
HEALTH_CORS = (ROOT / "backend" / "src" / "health_status.cpp").read_text(encoding="utf-8")
ACTIVE_CORS = (ROOT / "backend" / "src" / "http_security.cpp").read_text(encoding="utf-8")


def require(source: str, fragment: str, message: str) -> None:
    if fragment not in source:
        raise AssertionError(message)


def script_index(name: str) -> int:
    marker = f"'{name}'"
    index = CONFIG.find(marker)
    if index < 0:
        raise AssertionError(f"missing shared script: {name}")
    return index


require(CMAKE, "src/league_onboarding_hardening.cpp", "production target must compile onboarding hardening")
require(CMAKE, "src/snake_draft_only.cpp", "production target must compile the snake draft creation policy")
require(MIGRATION, "ADD COLUMN IF NOT EXISTS creation_key TEXT", "league creation keys must be persisted")
require(MIGRATION, "UNIQUE INDEX IF NOT EXISTS uq_leagues_account_creation_key", "creation keys must be unique per account")
require(MIGRATION, "WHERE creation_key IS NOT NULL AND creation_key <> ''", "empty compatibility keys must not collide")

require(BACKEND, "static const std::unordered_set<int> allowed{4, 6, 8, 10, 12, 14, 16}", "backend must enforce supported league sizes")
require(BACKEND, "SELECT pg_advisory_xact_lock(hashtextextended($1, 0))", "onboarding races must use database advisory locks")
require(BACKEND, 'lockKey(connection.get(), "create:" + email)', "league creation must lock per account")
require(BACKEND, 'lockKey(connection.get(), "join:" + leagueId)', "join, invite, and approval must lock per league")
require(BACKEND, "WHERE account_email = $1 AND creation_key = $2 LIMIT 1", "create retries must replay the confirmed league")
require(BACKEND, "SELECT COUNT(*) FROM leagues WHERE account_email = $1", "league limit must be checked inside the creation transaction")
require(BACKEND, '"league_limit_reached"', "league limit errors need a stable code")
require(BACKEND, "invites.size() > static_cast<std::size_t>(teams - 1)", "create invite count must respect league capacity")
require(BACKEND, "status IN ('active', 'pending')", "pending joins must reserve manager capacity")
require(BACKEND, '"league_full"', "full league races need a stable conflict code")
require(BACKEND, "currentStatus == \"pending\"", "duplicate join requests must be idempotent")
require(BACKEND, "status <> 'removed'", "invite capacity must count all current invited or reserved managers")
require(BACKEND, "WHERE league_id = $1 AND email = $2 AND status IN ('invited', 'pending')", "approval must use a compare-and-set transition")
require(BACKEND, '"join_request_conflict"', "approval races need a stable retryable conflict")
require(BACKEND, "registerSyncAdvice(onboardingAdvice)", "hardening must run before legacy route handlers")

require(SNAKE_BACKEND, 'request->getPath() != "/api/leagues"', "snake policy must be scoped to league creation")
require(SNAKE_BACKEND, 'draftType == "snake"', "backend must accept snake as the only explicit draft type")
require(SNAKE_BACKEND, '"unsupported_draft_type"', "unsupported draft submissions need a stable error code")
require(SNAKE_BACKEND, "Auction drafts are coming in a future release", "backend must explain auction availability")
require(SNAKE_BACKEND, "registerSyncAdvice(snakeDraftOnlyAdvice)", "snake policy must run before league creation handlers")
require(SNAKE_BACKEND, "init_priority(200)", "snake policy must register before legacy synchronous onboarding advice")

require(ROUTES, '"/api/leagues"', "create route must remain registered")
require(ROUTES, '"/api/leagues/{1}/join"', "join route must remain registered")
require(ROUTES, '"/api/leagues/{1}/members/{2}"', "member approval route must remain registered")
for cors_source in (HEALTH_CORS, ACTIVE_CORS):
    require(cors_source, "Authorization, Content-Type, X-Request-ID, Idempotency-Key", "browser preflight must allow onboarding operation keys")
    require(cors_source, '"GET, POST, PUT, PATCH, DELETE, OPTIONS"', "onboarding methods must remain available through CORS")

if not script_index("mutation-consistency.js") < script_index("snake-draft-only.js") < script_index("league-onboarding.js"):
    raise AssertionError("snake draft policy must load before the onboarding submit coordinator")
require(FRONTEND, "const ALLOWED_TEAM_COUNTS = Object.freeze([4, 6, 8, 10, 12, 14, 16])", "frontend must expose supported league sizes")
require(FRONTEND, "event.stopImmediatePropagation()", "new create coordinator must prevent legacy double submission")
require(FRONTEND, "'Idempotency-Key': operation.operationKey", "create and join requests must send stable operation keys")
require(FRONTEND, "existing?.fingerprint === fingerprint", "unchanged create retries must reuse one operation key")
require(FRONTEND, "The server may have accepted this request. Retry safely", "uncertain create outcomes must explain safe retry")
require(FRONTEND, "payload?.joinStatus === 'pending_approval'", "pending joins must remain pending instead of activating a league")
require(FRONTEND, "root.setActiveLeague?.(league.id)", "active joins must select the confirmed league")
require(FRONTEND, "form.dataset.onboardingBusy === 'true'", "duplicate form submissions must be blocked")

require(SNAKE_FRONTEND, "Auction (coming in future release)", "auction must remain visible as a future option")
require(SNAKE_FRONTEND, "button.disabled = true", "auction selection must be disabled")
require(SNAKE_FRONTEND, "input.value = SNAKE_DRAFT_TYPE", "league creation must force a snake draft payload")
require(SNAKE_FRONTEND, "addEventListener?.('submit', enforceSnakeDraft, true)", "snake enforcement must run in the capture phase")

print("league onboarding source contracts passed")
