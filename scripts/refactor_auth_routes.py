#!/usr/bin/env python3
"""Move /api/auth route registration from main.cpp into auth_routes.cpp."""

from pathlib import Path

root = Path(__file__).resolve().parents[1]
main_path = root / "backend/src/main.cpp"
cmake_path = root / "backend/CMakeLists.txt"


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected one source anchor, found {count}")
    return text.replace(old, new, 1)


def replace_between(text: str, start: str, end: str, replacement: str, label: str) -> str:
    start_index = text.find(start)
    if start_index < 0:
        raise RuntimeError(f"{label}: start marker not found")
    end_index = text.find(end, start_index)
    if end_index < 0:
        raise RuntimeError(f"{label}: end marker not found")
    return text[:start_index] + replacement + text[end_index:]


main = main_path.read_text(encoding="utf-8")
main = replace_once(
    main,
    '#include "auth_controller.h"\n',
    '#include "auth_controller.h"\n#include "auth_routes.h"\n',
    "include auth routes",
)

main = replace_between(
    main,
    "Json::Value authReadinessPayload() {",
    "bool hasBearerToken(const drogon::HttpRequestPtr &req, std::string &outToken) {",
    "",
    "move auth readiness and route availability helpers",
)

secure_route = '        .registerHandler("/api/secure/ping",'
route_start = '        .registerHandler("/api/auth/validate",'
admin_start = '        .registerHandler("/api/admin/ingest/cfbd",'
secure_index = main.find(secure_route)
if secure_index < 0:
    raise RuntimeError("secure route anchor not found")
route_index = main.find(route_start, secure_index)
admin_index = main.find(admin_start, route_index)
if route_index < 0 or admin_index < 0:
    raise RuntimeError("authentication route block anchors not found after secure route")
replacement = '''        ;

    cff::auth::registerAuthRoutes(app, jwtSecret);

    app.registerHandler("/api/admin/ingest/cfbd",'''
main = main[:route_index] + replacement + main[admin_index + len(admin_start):]

if "/api/auth/" in main:
    raise RuntimeError("main.cpp still contains an authentication route path")
if main.count("cff::auth::registerAuthRoutes(app, jwtSecret);") != 1:
    raise RuntimeError("main.cpp does not register the authentication route module exactly once")

main_path.write_text(main, encoding="utf-8")

cmake = cmake_path.read_text(encoding="utf-8")
cmake = replace_once(
    cmake,
    "    src/auth_controller.cpp\n    src/auth_account_store.cpp\n",
    "    src/auth_controller.cpp\n    src/auth_routes.cpp\n    src/auth_account_store.cpp\n",
    "add auth routes production source",
)
cmake_path.write_text(cmake, encoding="utf-8")

print("authentication route extraction applied")
