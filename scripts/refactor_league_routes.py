#!/usr/bin/env python3
"""Extract league route registration from backend/src/main.cpp."""

from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN_PATH = ROOT / "backend/src/main.cpp"
CMAKE_PATH = ROOT / "backend/CMakeLists.txt"
AUTH_CONTRACTS_PATH = ROOT / "scripts/auth_contract_tests.py"
HTTP_BOUNDARY_PATH = ROOT / "backend/tests/http_security_boundary_tests.py"
HEADER_PATH = ROOT / "backend/src/league_routes.h"
SOURCE_PATH = ROOT / "backend/src/league_routes.cpp"
BOUNDARY_PATH = ROOT / "backend/tests/league_routes_boundary_tests.py"


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected one source anchor, found {count}")
    return text.replace(old, new, 1)


main = MAIN_PATH.read_text(encoding="utf-8")

main = replace_once(
    main,
    '#include "operations_routes.h"\n',
    '#include "operations_routes.h"\n#include "league_routes.h"\n',
    "add league route include",
)
main = replace_once(
    main,
    '#include "league_models.h"\n',
    "",
    "remove league model include from main",
)
main = replace_once(
    main,
    '#include "handlers/league_handler.h"\n',
    "",
    "remove league handler include from main",
)

param_start = main.find("    auto preflightOneParamHandler =")
param_end = main.find("    const bool ingestOnStartup", param_start)
if param_start < 0 or param_end < 0:
    raise RuntimeError("parameterized preflight helpers: source anchors not found")
main = main[:param_start] + main[param_end:]

normal_start = main.find('    app.registerHandler("/api/leagues",')
normal_end_marker = '        .registerHandler("/api/scores/live",'
normal_end = main.find(normal_end_marker, normal_start)
if normal_start < 0 or normal_end < 0:
    raise RuntimeError("league route block: source anchors not found")
normal_routes = main[normal_start:normal_end].rstrip()
main = (
    main[:normal_start]
    + "    cff::league::registerLeagueRoutes(app, jwtSecret, allowedOrigins);\n\n"
    + '    app.registerHandler("/api/scores/live",'
    + main[normal_end + len(normal_end_marker):]
)

preflight_start = main.find(
    '        .registerHandler("/api/leagues", preflightHandler, {drogon::Options})'
)
preflight_end_marker = (
    '        .registerHandler("/api/scores/live", preflightHandler, {drogon::Options})'
)
preflight_end = main.find(preflight_end_marker, preflight_start)
if preflight_start < 0 or preflight_end < 0:
    raise RuntimeError("league preflight block: source anchors not found")
preflight_routes = main[preflight_start:preflight_end].rstrip()
main = main[:preflight_start] + main[preflight_end:]

for leaked in (
    '"/api/leagues',
    "cff::handlers::handle",
    "preflightOneParamHandler",
    "preflightTwoParamHandler",
):
    if leaked in main:
        raise RuntimeError(f"main.cpp still contains moved league route symbol: {leaked}")
if main.count(
    "cff::league::registerLeagueRoutes(app, jwtSecret, allowedOrigins);"
) != 1:
    raise RuntimeError("main.cpp must register league routes exactly once")

MAIN_PATH.write_text(main, encoding="utf-8")

header = """#pragma once

#include <drogon/drogon.h>

#include <optional>
#include <string>
#include <unordered_set>

namespace cff::league {

void registerLeagueRoutes(
    drogon::HttpAppFramework &app,
    const std::optional<std::string> &jwtSecret,
    const std::unordered_set<std::string> &allowedOrigins);

} // namespace cff::league
"""
HEADER_PATH.write_text(header, encoding="utf-8")

source = f'''#include "league_routes.h"

#include "handlers/league_handler.h"
#include "http_security.h"

#include <utility>

namespace cff::league {{

void registerLeagueRoutes(
    drogon::HttpAppFramework &app,
    const std::optional<std::string> &jwtSecret,
    const std::unordered_set<std::string> &allowedOrigins) {{
{normal_routes}
        ;

    const auto preflightHandler = [allowedOrigins](
        const drogon::HttpRequestPtr &request,
        std::function<void(const drogon::HttpResponsePtr &)> &&callback) {{
        callback(cff::http::buildPreflightResponse(request, allowedOrigins));
    }};
    const auto preflightOneParamHandler = [allowedOrigins](
        const drogon::HttpRequestPtr &request,
        std::function<void(const drogon::HttpResponsePtr &)> &&callback,
        const std::string &) {{
        callback(cff::http::buildPreflightResponse(request, allowedOrigins));
    }};
    const auto preflightTwoParamHandler = [allowedOrigins](
        const drogon::HttpRequestPtr &request,
        std::function<void(const drogon::HttpResponsePtr &)> &&callback,
        const std::string &,
        const std::string &) {{
        callback(cff::http::buildPreflightResponse(request, allowedOrigins));
    }};

    app.registerHandler{preflight_routes[len("        .registerHandler"):]}
        ;
}}

}} // namespace cff::league
'''
SOURCE_PATH.write_text(source, encoding="utf-8")

cmake = CMAKE_PATH.read_text(encoding="utf-8")
cmake = replace_once(
    cmake,
    "    src/operations_routes.cpp\n",
    "    src/operations_routes.cpp\n    src/league_routes.cpp\n",
    "register league route production source",
)
CMAKE_PATH.write_text(cmake, encoding="utf-8")

http_boundary = HTTP_BOUNDARY_PATH.read_text(encoding="utf-8")
http_boundary = replace_once(
    http_boundary,
    'operations_routes = (root / "backend/src/operations_routes.cpp").read_text(encoding="utf-8")\n',
    'operations_routes = (root / "backend/src/operations_routes.cpp").read_text(encoding="utf-8")\n'
    'league_routes = (root / "backend/src/league_routes.cpp").read_text(encoding="utf-8")\n',
    "load league routes in shared security boundary",
)
http_boundary = replace_once(
    http_boundary,
    '    if implementation in operations_routes:\n'
    '        raise SystemExit(f"shared HTTP security implementation leaked into operations_routes.cpp: {implementation}")\n',
    '    if implementation in operations_routes:\n'
    '        raise SystemExit(f"shared HTTP security implementation leaked into operations_routes.cpp: {implementation}")\n'
    '    if implementation in league_routes:\n'
    '        raise SystemExit(f"shared HTTP security implementation leaked into league_routes.cpp: {implementation}")\n',
    "check league route security implementation leaks",
)
http_boundary = replace_once(
    http_boundary,
    'required_main_delegation = (\n'
    '    "cff::http::applyCorsHeaders(req, resp, allowedOrigins)",\n'
    '    "cff::http::buildPreflightResponse(req, allowedOrigins)",\n'
    '    "cff::http::requireAccount(req, callback, jwtSecret, accountEmail)",\n'
    ')\n',
    'required_main_delegation = (\n'
    '    "cff::http::applyCorsHeaders(req, resp, allowedOrigins)",\n'
    '    "cff::http::buildPreflightResponse(req, allowedOrigins)",\n'
    ')\n',
    "remove moved league account guard from main contract",
)
league_security_block = '''
required_league_route_delegation = (
    "cff::http::requireAccount(req, callback, jwtSecret, accountEmail)",
    "cff::http::buildPreflightResponse(request, allowedOrigins)",
)
for delegation in required_league_route_delegation:
    if delegation not in league_routes:
        raise SystemExit(f"league_routes.cpp HTTP security delegation missing: {delegation}")

'''
http_boundary = replace_once(
    http_boundary,
    "required_behavior = (\n",
    league_security_block + "required_behavior = (\n",
    "add league route security delegation contracts",
)
HTTP_BOUNDARY_PATH.write_text(http_boundary, encoding="utf-8")

normal_contracts = [
    ("/api/leagues", "handleListLeagues", "{drogon::Get}"),
    ("/api/leagues", "handleCreateLeague", "{drogon::Post}"),
    ("/api/leagues/{1}", "handleGetLeague", "{drogon::Get}"),
    ("/api/leagues/{1}", "handleUpdateLeague", "{drogon::Put}"),
    ("/api/leagues/{1}", "handleDeleteLeague", "{drogon::Delete}"),
    ("/api/leagues/{1}/members", "handleListMembers", "{drogon::Get}"),
    ("/api/leagues/{1}/members", "handleInviteMember", "{drogon::Post}"),
    ("/api/leagues/{1}/members/{2}", "handleUpdateMember", "{drogon::Put, drogon::Post}"),
    ("/api/leagues/{1}/join", "handleJoinLeague", "{drogon::Post}"),
    ("/api/leagues/{1}/roster", "handleGetRoster", "{drogon::Get}"),
    ("/api/leagues/{1}/rosters/{2}", "handleGetManagerRoster", "{drogon::Get}"),
    ("/api/leagues/{1}/roster", "handleAddRosterPlayer", "{drogon::Post}"),
    ("/api/leagues/{1}/roster/drop", "handleDropRosterPlayer", "{drogon::Post}"),
    ("/api/leagues/{1}/roster/{2}/slot", "handleUpdateRosterSlot", "{drogon::Post, drogon::Put}"),
    ("/api/leagues/{1}/free-agents", "handleFreeAgents", "{drogon::Get}"),
    ("/api/leagues/{1}/draft", "handleGetDraftState", "{drogon::Get}"),
    ("/api/leagues/{1}/draft/queue", "handleSaveDraftQueue", "{drogon::Put, drogon::Post}"),
    ("/api/leagues/{1}/draft/order", "handleSaveDraftOrder", "{drogon::Put, drogon::Post}"),
    ("/api/leagues/{1}/draft/picks", "handleMakeDraftPick", "{drogon::Post}"),
    ("/api/leagues/{1}/draft/reset", "handleResetDraft", "{drogon::Post}"),
    ("/api/leagues/{1}/draft/undo", "handleUndoDraftPick", "{drogon::Post}"),
    ("/api/leagues/{1}/waivers", "handleListWaivers", "{drogon::Get}"),
    ("/api/leagues/{1}/waivers", "handleCreateWaiver", "{drogon::Post}"),
    ("/api/leagues/{1}/waivers/process", "handleProcessWaivers", "{drogon::Post}"),
    ("/api/leagues/{1}/waivers/{2}/process", "handleProcessWaiver", "{drogon::Post}"),
    ("/api/leagues/{1}/waivers/{2}/status", "handleUpdateWaiverStatus", "{drogon::Post}"),
    ("/api/leagues/{1}/waivers/reorder", "handleReorderWaivers", "{drogon::Post}"),
    ("/api/leagues/{1}/waiver-priority", "handleListWaiverPriority", "{drogon::Get}"),
    ("/api/leagues/{1}/waiver-priority/reset", "handleResetWaiverPriority", "{drogon::Post}"),
    ("/api/leagues/{1}/trades", "handleListTrades", "{drogon::Get}"),
    ("/api/leagues/{1}/trades", "handleCreateTrade", "{drogon::Post}"),
    ("/api/leagues/{1}/trades/{2}/status", "handleUpdateTradeStatus", "{drogon::Post}"),
    ("/api/leagues/{1}/matchups", "handleListMatchups", "{drogon::Get}"),
    ("/api/leagues/{1}/matchups/generate", "handleGenerateMatchups", "{drogon::Post}"),
    ("/api/leagues/{1}/matchups/generate-season", "handleGenerateSeasonSchedule", "{drogon::Post}"),
    ("/api/leagues/{1}/score/week/{2}", "handleScoreWeek", "{drogon::Post}"),
    ("/api/leagues/{1}/score/week/{2}/finalize", "handleFinalizeWeek", "{drogon::Post}"),
    ("/api/leagues/{1}/transactions", "handleListTransactions", "{drogon::Get}"),
    ("/api/leagues/{1}/feed", "handleListLeagueFeed", "{drogon::Get}"),
    ("/api/leagues/{1}/feed/posts", "handleCreateLeagueFeedPost", "{drogon::Post}"),
]
unique_paths = list(dict.fromkeys(path for path, _, _ in normal_contracts))
normal_counts = Counter(path for path, _, _ in normal_contracts)

boundary = f'''#!/usr/bin/env python3
from collections import Counter
from pathlib import Path

root = Path(__file__).resolve().parents[2]
main = (root / "backend/src/main.cpp").read_text(encoding="utf-8")
routes = (root / "backend/src/league_routes.cpp").read_text(encoding="utf-8")
header = (root / "backend/src/league_routes.h").read_text(encoding="utf-8")
handlers = (root / "backend/src/handlers/league_handler.cpp").read_text(encoding="utf-8")
cmake = (root / "backend/CMakeLists.txt").read_text(encoding="utf-8")

normal_contracts = {normal_contracts!r}
unique_paths = {unique_paths!r}
normal_counts = Counter(path for path, _, _ in normal_contracts)

if '"/api/leagues' in main:
    raise SystemExit("league route registration leaked into main.cpp")
if "cff::handlers::handle" in main:
    raise SystemExit("league handler delegation leaked into main.cpp")
if main.count(
    "cff::league::registerLeagueRoutes(app, jwtSecret, allowedOrigins);"
) != 1:
    raise SystemExit("main.cpp must register league routes exactly once")
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

print("league route boundary contracts passed")
'''
BOUNDARY_PATH.write_text(boundary, encoding="utf-8")

contracts = AUTH_CONTRACTS_PATH.read_text(encoding="utf-8")
league_runtime_contracts = r'''
    league_missing_token = expect(call("GET", "/api/leagues"), 401, "league list without token")
    require(
        league_missing_token.get("error") == "Unauthorized",
        f"league missing-token response changed: {league_missing_token!r}",
    )
    league_two_param_missing_token = expect(
        call("GET", "/api/leagues/missing/rosters/manager@example.test"),
        401,
        "two-parameter league route without token",
    )
    require(
        league_two_param_missing_token.get("error") == "Unauthorized",
        f"two-parameter league authorization changed: {league_two_param_missing_token!r}",
    )

    league_name = f"Auth Contract League {time.time_ns()}"
    created_league = expect(
        call(
            "POST",
            "/api/leagues",
            token=session,
            payload={"name": league_name, "teams": 4},
        ),
        201,
        "create league",
    )
    league_id = str(created_league.get("id", ""))
    require(league_id, f"created league id missing: {created_league!r}")
    require(created_league.get("name") == league_name, f"league name changed: {created_league!r}")
    require(created_league.get("teams") == 4, f"four-team league contract changed: {created_league!r}")

    leagues = expect(call("GET", "/api/leagues", token=session), 200, "list leagues")
    require(
        isinstance(leagues, list) and any(item.get("id") == league_id for item in leagues),
        f"created league absent from list: {leagues!r}",
    )
    fetched_league = expect(
        call("GET", f"/api/leagues/{league_id}", token=session),
        200,
        "get league",
    )
    require(fetched_league.get("id") == league_id, f"league fetch changed: {fetched_league!r}")

    updated_name = league_name + " Updated"
    update_payload = dict(fetched_league)
    update_payload["name"] = updated_name
    updated_league = expect(
        call("PUT", f"/api/leagues/{league_id}", token=session, payload=update_payload),
        200,
        "update league",
    )
    require(updated_league.get("name") == updated_name, f"league update changed: {updated_league!r}")

    roster = expect(call("GET", f"/api/leagues/{league_id}/roster", token=session), 200, "league roster")
    require(isinstance(roster, list), f"league roster is not an array: {roster!r}")
    manager_roster = expect(
        call("GET", f"/api/leagues/{league_id}/rosters/{EMAIL}", token=session),
        200,
        "manager roster",
    )
    require(isinstance(manager_roster, list), f"manager roster is not an array: {manager_roster!r}")

    for path, label in (
        (f"/api/leagues/{league_id}/waivers", "league waivers"),
        (f"/api/leagues/{league_id}/trades", "league trades"),
        (f"/api/leagues/{league_id}/transactions", "league transactions"),
        (f"/api/leagues/{league_id}/feed", "league feed"),
    ):
        collection = expect(call("GET", path, token=session), 200, label)
        require(isinstance(collection, list), f"{label} is not an array: {collection!r}")

    league_preflight_paths = (
        "/api/leagues",
        f"/api/leagues/{league_id}",
        f"/api/leagues/{league_id}/members",
        f"/api/leagues/{league_id}/members/member@example.test",
        f"/api/leagues/{league_id}/join",
        f"/api/leagues/{league_id}/roster",
        f"/api/leagues/{league_id}/rosters/{EMAIL}",
        f"/api/leagues/{league_id}/roster/drop",
        f"/api/leagues/{league_id}/roster/player-1/slot",
        f"/api/leagues/{league_id}/free-agents",
        f"/api/leagues/{league_id}/draft",
        f"/api/leagues/{league_id}/draft/queue",
        f"/api/leagues/{league_id}/draft/order",
        f"/api/leagues/{league_id}/draft/picks",
        f"/api/leagues/{league_id}/draft/reset",
        f"/api/leagues/{league_id}/draft/undo",
        f"/api/leagues/{league_id}/waivers",
        f"/api/leagues/{league_id}/waivers/process",
        f"/api/leagues/{league_id}/waivers/claim-1/process",
        f"/api/leagues/{league_id}/waivers/claim-1/status",
        f"/api/leagues/{league_id}/waivers/reorder",
        f"/api/leagues/{league_id}/waiver-priority",
        f"/api/leagues/{league_id}/waiver-priority/reset",
        f"/api/leagues/{league_id}/trades",
        f"/api/leagues/{league_id}/trades/trade-1/status",
        f"/api/leagues/{league_id}/matchups",
        f"/api/leagues/{league_id}/matchups/generate",
        f"/api/leagues/{league_id}/matchups/generate-season",
        f"/api/leagues/{league_id}/score/week/1",
        f"/api/leagues/{league_id}/score/week/1/finalize",
        f"/api/leagues/{league_id}/transactions",
        f"/api/leagues/{league_id}/feed",
        f"/api/leagues/{league_id}/feed/posts",
    )
    require(len(league_preflight_paths) == 33, "league preflight coverage list changed")
    for league_path in league_preflight_paths:
        league_preflight = call(
            "OPTIONS",
            league_path,
            headers={
                "Access-Control-Request-Method": "POST",
                "Access-Control-Request-Headers": "content-type, authorization",
            },
        )
        require(
            league_preflight.status == 204,
            f"league preflight failed for {league_path}: {league_preflight.status}",
        )
        require(
            league_preflight.headers.get("access-control-allow-origin") == ORIGIN,
            f"league CORS origin wrong for {league_path}: {league_preflight.headers!r}",
        )

    deleted_league = expect(
        call("DELETE", f"/api/leagues/{league_id}", token=session),
        200,
        "delete league",
    )
    require(deleted_league.get("deleted") is True, f"league delete changed: {deleted_league!r}")
    expect(call("GET", f"/api/leagues/{league_id}", token=session), 404, "deleted league lookup")
    leagues_after_delete = expect(call("GET", "/api/leagues", token=session), 200, "list after league delete")
    require(
        all(item.get("id") != league_id for item in leagues_after_delete),
        f"deleted league still listed: {leagues_after_delete!r}",
    )

'''
contracts = replace_once(
    contracts,
    '    duplicate = expect(call("POST", "/api/auth/signup", payload={"email": EMAIL.upper(), "password": PASSWORD}), 202, "duplicate signup")\n',
    league_runtime_contracts
    + '    duplicate = expect(call("POST", "/api/auth/signup", payload={"email": EMAIL.upper(), "password": PASSWORD}), 202, "duplicate signup")\n',
    "add league runtime contracts",
)
contracts = replace_once(
    contracts,
    '        "operationsCors": True,\n',
    '        "operationsCors": True,\n'
    '        "leagueRoutes": True,\n'
    '        "leagueAuthorization": True,\n'
    '        "leagueLifecycle": True,\n'
    '        "leagueCors": True,\n',
    "report league contract coverage",
)
AUTH_CONTRACTS_PATH.write_text(contracts, encoding="utf-8")

print("league route extraction applied")
