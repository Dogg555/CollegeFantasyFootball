#!/usr/bin/env python3
from collections import Counter
from pathlib import Path

root = Path(__file__).resolve().parents[2]
main = (root / "backend/src/main.cpp").read_text(encoding="utf-8")
composition = (root / "backend/src/app_composition.cpp").read_text(encoding="utf-8")
routes = (root / "backend/src/league_routes.cpp").read_text(encoding="utf-8")
header = (root / "backend/src/league_routes.h").read_text(encoding="utf-8")
handlers = (root / "backend/src/handlers/league_handler.cpp").read_text(encoding="utf-8")
cmake = (root / "backend/CMakeLists.txt").read_text(encoding="utf-8")

normal_contracts = [('/api/leagues', 'handleListLeagues', '{drogon::Get}'), ('/api/leagues', 'handleCreateLeague', '{drogon::Post}'), ('/api/leagues/{1}', 'handleGetLeague', '{drogon::Get}'), ('/api/leagues/{1}', 'handleUpdateLeague', '{drogon::Put}'), ('/api/leagues/{1}', 'handleDeleteLeague', '{drogon::Delete}'), ('/api/leagues/{1}/members', 'handleListMembers', '{drogon::Get}'), ('/api/leagues/{1}/members', 'handleInviteMember', '{drogon::Post}'), ('/api/leagues/{1}/members/{2}', 'handleUpdateMember', '{drogon::Put, drogon::Post}'), ('/api/leagues/{1}/join', 'handleJoinLeague', '{drogon::Post}'), ('/api/leagues/{1}/roster', 'handleGetRoster', '{drogon::Get}'), ('/api/leagues/{1}/rosters/{2}', 'handleGetManagerRoster', '{drogon::Get}'), ('/api/leagues/{1}/roster', 'handleAddRosterPlayer', '{drogon::Post}'), ('/api/leagues/{1}/roster/drop', 'handleDropRosterPlayer', '{drogon::Post}'), ('/api/leagues/{1}/roster/{2}/slot', 'handleUpdateRosterSlot', '{drogon::Post, drogon::Put}'), ('/api/leagues/{1}/free-agents', 'handleFreeAgents', '{drogon::Get}'), ('/api/leagues/{1}/draft', 'handleGetDraftState', '{drogon::Get}'), ('/api/leagues/{1}/draft/queue', 'handleSaveDraftQueue', '{drogon::Put, drogon::Post}'), ('/api/leagues/{1}/draft/order', 'handleSaveDraftOrder', '{drogon::Put, drogon::Post}'), ('/api/leagues/{1}/draft/picks', 'handleMakeDraftPick', '{drogon::Post}'), ('/api/leagues/{1}/draft/reset', 'handleResetDraft', '{drogon::Post}'), ('/api/leagues/{1}/draft/undo', 'handleUndoDraftPick', '{drogon::Post}'), ('/api/leagues/{1}/waivers', 'handleListWaivers', '{drogon::Get}'), ('/api/leagues/{1}/waivers', 'handleCreateWaiver', '{drogon::Post}'), ('/api/leagues/{1}/waivers/process', 'handleProcessWaivers', '{drogon::Post}'), ('/api/leagues/{1}/waivers/{2}/process', 'handleProcessWaiver', '{drogon::Post}'), ('/api/leagues/{1}/waivers/{2}/status', 'handleUpdateWaiverStatus', '{drogon::Post}'), ('/api/leagues/{1}/waivers/reorder', 'handleReorderWaivers', '{drogon::Post}'), ('/api/leagues/{1}/waiver-priority', 'handleListWaiverPriority', '{drogon::Get}'), ('/api/leagues/{1}/waiver-priority/reset', 'handleResetWaiverPriority', '{drogon::Post}'), ('/api/leagues/{1}/trades', 'handleListTrades', '{drogon::Get}'), ('/api/leagues/{1}/trades', 'handleCreateTrade', '{drogon::Post}'), ('/api/leagues/{1}/trades/{2}/status', 'handleUpdateTradeStatus', '{drogon::Post}'), ('/api/leagues/{1}/matchups', 'handleListMatchups', '{drogon::Get}'), ('/api/leagues/{1}/matchups/generate', 'handleGenerateMatchups', '{drogon::Post}'), ('/api/leagues/{1}/matchups/generate-season', 'handleGenerateSeasonSchedule', '{drogon::Post}'), ('/api/leagues/{1}/score/week/{2}', 'handleScoreWeek', '{drogon::Post}'), ('/api/leagues/{1}/score/week/{2}/finalize', 'handleFinalizeWeek', '{drogon::Post}'), ('/api/leagues/{1}/transactions', 'handleListTransactions', '{drogon::Get}'), ('/api/leagues/{1}/feed', 'handleListLeagueFeed', '{drogon::Get}'), ('/api/leagues/{1}/feed/posts', 'handleCreateLeagueFeedPost', '{drogon::Post}')]
unique_paths = ['/api/leagues', '/api/leagues/{1}', '/api/leagues/{1}/members', '/api/leagues/{1}/members/{2}', '/api/leagues/{1}/join', '/api/leagues/{1}/roster', '/api/leagues/{1}/rosters/{2}', '/api/leagues/{1}/roster/drop', '/api/leagues/{1}/roster/{2}/slot', '/api/leagues/{1}/free-agents', '/api/leagues/{1}/draft', '/api/leagues/{1}/draft/queue', '/api/leagues/{1}/draft/order', '/api/leagues/{1}/draft/picks', '/api/leagues/{1}/draft/reset', '/api/leagues/{1}/draft/undo', '/api/leagues/{1}/waivers', '/api/leagues/{1}/waivers/process', '/api/leagues/{1}/waivers/{2}/process', '/api/leagues/{1}/waivers/{2}/status', '/api/leagues/{1}/waivers/reorder', '/api/leagues/{1}/waiver-priority', '/api/leagues/{1}/waiver-priority/reset', '/api/leagues/{1}/trades', '/api/leagues/{1}/trades/{2}/status', '/api/leagues/{1}/matchups', '/api/leagues/{1}/matchups/generate', '/api/leagues/{1}/matchups/generate-season', '/api/leagues/{1}/score/week/{2}', '/api/leagues/{1}/score/week/{2}/finalize', '/api/leagues/{1}/transactions', '/api/leagues/{1}/feed', '/api/leagues/{1}/feed/posts']
normal_counts = Counter(path for path, _, _ in normal_contracts)

if '"/api/leagues' in main:
    raise SystemExit("league route registration leaked into main.cpp")
if "cff::handlers::handle" in main:
    raise SystemExit("league handler delegation leaked into main.cpp")
if "cff::league::registerLeagueRoutes(" in main:
    raise SystemExit("main.cpp must delegate league registration through app composition")
if composition.count(
    "cff::league::registerLeagueRoutes(app, jwtSecret, allowedOrigins);"
) != 1:
    raise SystemExit("application composition must register league routes exactly once")
if "void registerLeagueRoutes(" not in header:
    raise SystemExit("league_routes.h declaration missing")
if "void registerLeagueRoutes(" not in routes:
    raise SystemExit("league_routes.cpp implementation missing")

cursor = 0
for path, handler, methods in normal_contracts:
    path_index = routes.find(f'"{path}"', cursor)
    if path_index < 0:
        raise SystemExit(f"league route registration missing: {path} -> {handler}")
    next_registration = routes.find(".registerHandler(", path_index + 1)
    if next_registration < 0:
        next_registration = len(routes)
    handler_index = routes.find(f"cff::handlers::{handler}", path_index)
    if handler_index < 0 or handler_index > next_registration:
        raise SystemExit(f"league handler delegation missing: {path} -> {handler}")
    method_index = routes.find(methods, handler_index)
    if method_index < 0 or method_index > next_registration:
        raise SystemExit(f"league method contract changed: {path} -> {handler} {methods}")
    if f"void {handler}(" not in handlers:
        raise SystemExit(f"league business handler implementation missing: {handler}")
    cursor = path_index + 1

for path in unique_paths:
    expected_count = normal_counts[path] + 1
    actual_count = routes.count(f'"{path}"')
    if actual_count != expected_count:
        raise SystemExit(
            f"league route count changed for {path}: expected {expected_count}, got {actual_count}"
        )
    placeholders = path.count("{")
    preflight_name = (
        "preflightHandler"
        if placeholders == 0
        else "preflightOneParamHandler"
        if placeholders == 1
        else "preflightTwoParamHandler"
    )
    contract = f'"{path}", {preflight_name}, {{drogon::Options}}'
    if contract not in routes:
        raise SystemExit(f"league preflight contract missing: {contract}")

if routes.count("cff::http::requireAccount(") != len(normal_contracts):
    raise SystemExit("every league business route must retain the account guard")
if routes.count("{drogon::Options}") != len(unique_paths):
    raise SystemExit("every unique league route must retain one OPTIONS registration")
if "cff::http::buildPreflightResponse(request, allowedOrigins)" not in routes:
    raise SystemExit("league preflight must delegate to shared HTTP security")
if "src/league_routes.cpp" not in cmake:
    raise SystemExit("league_routes.cpp is not part of the production target")
if "src/app_composition.cpp" not in cmake:
    raise SystemExit("app_composition.cpp is not part of the production target")

print("league route boundary contracts passed")
