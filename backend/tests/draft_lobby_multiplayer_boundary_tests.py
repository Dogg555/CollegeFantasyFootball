#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def require(condition, message):
    if not condition:
        raise AssertionError(message)


paths = {
    "routes": ROOT / "backend/src/league_routes.cpp",
    "header": ROOT / "backend/src/handlers/league_handler.h",
    "handler": ROOT / "backend/src/handlers/league_handler.cpp",
    "beta_backend": ROOT / "backend/src/league_beta_stability.cpp",
    "state": ROOT / "frontend/state.js",
    "draft": ROOT / "frontend/draft.js",
    "draft_html": ROOT / "frontend/draft.html",
    "league": ROOT / "frontend/league.js",
    "beta_frontend": ROOT / "frontend/league-beta-stability.js",
    "integration": ROOT / "scripts/draft_lobby_multiplayer_tests.py",
    "workflow": ROOT / ".github/workflows/draft-lobby-multiplayer.yml",
}
for path in paths.values():
    require(path.exists(), f"missing required file: {path.relative_to(ROOT)}")

routes = paths["routes"].read_text(encoding="utf-8")
header = paths["header"].read_text(encoding="utf-8")
handler = paths["handler"].read_text(encoding="utf-8")
beta_backend = paths["beta_backend"].read_text(encoding="utf-8")
state = paths["state"].read_text(encoding="utf-8")
draft = paths["draft"].read_text(encoding="utf-8")
draft_html = paths["draft_html"].read_text(encoding="utf-8")
league = paths["league"].read_text(encoding="utf-8")
beta_frontend = paths["beta_frontend"].read_text(encoding="utf-8")
integration = paths["integration"].read_text(encoding="utf-8")
workflow = paths["workflow"].read_text(encoding="utf-8")

start_path = '"/api/leagues/{1}/draft/start"'
require(routes.count(start_path) == 2, "draft start must have one POST handler and one OPTIONS handler")
require("handleStartDraft(req, std::move(callback), accountEmail, leagueId)" in routes,
        "draft start route does not delegate to the authenticated handler")
require("{drogon::Post}" in routes[routes.index(start_path):routes.index(start_path) + 1200],
        "draft start route is not POST-only")
require(f".registerHandler({start_path}, preflightOneParamHandler, {{drogon::Options}})" in routes,
        "draft start route is missing shared CORS preflight")
require("void handleStartDraft" in header, "draft start handler is not declared")
require("void handleStartDraft" in handler, "draft start handler is not implemented")
require("dbStartDraft" in handler, "production draft start persistence is missing")

require("'not_started'" in handler, "backend does not model a waiting draft state")
require("payload[\"lobbyOpen\"]" in handler, "draft state does not expose authoritative lobby access")
require("payload[\"startedAt\"]" in handler, "draft state does not expose a shared start timestamp")
require("status = 'open'" in handler and "pick_deadline" in handler,
        "explicit start does not activate the shared draft clock")
require("status = 'not_started'" in handler and "started_at = NULL" in handler,
        "draft reset does not return to the waiting lobby state")
require("!dbDraftLobbyOpen(conn.get(), leagueId)" in handler,
        "production picks are not guarded by the lobby state")
require("currentStatus != \"open\"" in handler,
        "production picks are not guarded by the explicit live state")
require("status = 'active'" in handler,
        "draft order is not restricted to active league managers")

require('app.registerHandler("/api/leagues/{1}/settings", handleSettings' in beta_backend,
        "the verified league settings endpoint is missing")
require('draft_lobby_open = $9::boolean' in beta_backend,
        "the verified settings endpoint does not persist the lobby-open flag")
require('draft_lobby_started_at = NULLIF($10, \'\')::timestamptz' in beta_backend,
        "the verified settings endpoint does not persist the lobby-open timestamp")
require('status = \'active\'' in beta_backend and 'status == "invited"' in beta_backend,
        "invited users are not activated through the real join flow")

require("status: 'not_started'" in state, "frontend draft metadata still defaults to live")
require("async function startDraftApi" in state, "frontend has no authenticated start call")
require("/draft/start" in state, "frontend start call does not use the new endpoint")
require("state.lobbyOpen" in state and "draftLobbyOpen" in state,
        "authoritative lobby state is not written back to the league cache")
require("if (meta.status !== 'open')" in state,
        "local draft picks can still be made before explicit start")

require("draft-start" in draft_html, "draft room has no commissioner start control")
require("draft-lobby-copy" in draft_html, "draft room has no waiting-state explanation")
require("startDraftApi" in draft, "draft start control is not wired")
require("startDraftSyncPolling" in draft, "draft room does not poll shared multiplayer state")
require("setInterval" in draft and "2000" in draft,
        "draft room multiplayer polling cadence is missing")
require("if (!getAuthState()?.token) return;" in draft,
        "draft refresh is not authentication guarded")
refresh_start = draft.index("async function refreshDraftFromApi")
refresh_end = draft.index("async function refreshDraftLeagueShell", refresh_start)
refresh_body = draft[refresh_start:refresh_end]
require("if (!canEnterDraftRoom()) return;" not in refresh_body,
        "stale local lobby state still blocks authoritative draft refresh")
require("member.status || '').toLowerCase() === 'active'" in draft,
        "frontend draft order still includes invited or pending users")
require("draftLobbyLink.hidden" in league and "draftLobbyOpen" in league,
        "league lobby entry link is not access-aware")
require("'draftLobbyOpen'" in beta_frontend and "'draftLobbyStartedAt'" in beta_frontend,
        "verified settings do not compare the persisted draft lobby fields")

for required in (
    "/draft/start",
    "/leagues/join",
    "/settings",
    "crossUserPickSync",
    "wrong_email",
    "not_started",
    "lobbyOpen",
):
    require(required in integration, f"integration contract is missing {required}")

require("branches: [Test]" in workflow, "draft lobby workflow does not target Test")
require("docker build -f backend/Dockerfile" in workflow, "workflow does not build the production API image")
require("postgres:16" in workflow, "workflow does not use PostgreSQL 16")
require("league-beta-stability.js" in workflow,
        "workflow does not track or syntax-check the verified settings client")
require("draft_lobby_multiplayer_tests.py" in workflow, "workflow does not run the multiplayer integration contract")
require("draft_lobby_multiplayer_boundary_tests.py" in workflow, "workflow does not run the ownership boundary")

print("draft lobby multiplayer boundary contracts passed")
