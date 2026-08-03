#!/usr/bin/env python3
"""Move /api/auth route registration from main.cpp into auth_routes.cpp."""

from pathlib import Path

root = Path(__file__).resolve().parents[1]
main_path = root / "backend/src/main.cpp"
routes_path = root / "backend/src/auth_routes.cpp"
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

    cff::auth::registerAuthRoutes(app, jwtSecret, allowedOrigins);

    app.registerHandler("/api/admin/ingest/cfbd",'''
main = main[:route_index] + replacement + main[admin_index + len(admin_start):]

preflight_secure = '        .registerHandler("/api/secure/ping", preflightHandler, {drogon::Options})\n'
auth_preflight_start = '        .registerHandler("/api/auth/validate", preflightHandler, {drogon::Options})\n'
league_preflight_start = '        .registerHandler("/api/leagues", preflightHandler, {drogon::Options})\n'
preflight_secure_index = main.find(preflight_secure)
preflight_auth_index = main.find(auth_preflight_start, preflight_secure_index)
preflight_league_index = main.find(league_preflight_start, preflight_auth_index)
if preflight_secure_index < 0 or preflight_auth_index < 0 or preflight_league_index < 0:
    raise RuntimeError("authentication preflight route block anchors not found")
main = main[:preflight_auth_index] + main[preflight_league_index:]

if "/api/auth/" in main:
    raise RuntimeError("main.cpp still contains an authentication route path")
if main.count("cff::auth::registerAuthRoutes(app, jwtSecret, allowedOrigins);") != 1:
    raise RuntimeError("main.cpp does not register the authentication route module exactly once")

main_path.write_text(main, encoding="utf-8")

routes = routes_path.read_text(encoding="utf-8")
routes = replace_once(
    routes,
    "#include <string>\n",
    "#include <string>\n#include <unordered_set>\n",
    "include route origin set",
)

routes = replace_once(
    routes,
    '''    return emailForSessionToken(*token).has_value();
}

} // namespace

void registerAuthRoutes(drogon::HttpAppFramework &app,
                        const std::optional<std::string> &jwtSecret) {
''',
    '''    return emailForSessionToken(*token).has_value();
}

void applyCorsHeaders(const drogon::HttpRequestPtr &req,
                      const drogon::HttpResponsePtr &resp,
                      const std::unordered_set<std::string> &allowedOrigins) {
    const auto origin = req->getHeader("Origin");
    if (!allowedOrigins.empty() && allowedOrigins.find(origin) != allowedOrigins.end()) {
        resp->addHeader("Access-Control-Allow-Origin", origin);
        resp->addHeader("Vary", "Origin");
    }
    resp->addHeader("Access-Control-Allow-Headers", "Authorization, Content-Type");
    resp->addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
}

drogon::HttpResponsePtr buildPreflightResponse(
    const drogon::HttpRequestPtr &req,
    const std::unordered_set<std::string> &allowedOrigins) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    applyCorsHeaders(req, resp, allowedOrigins);
    resp->setStatusCode(drogon::k204NoContent);
    return resp;
}

} // namespace

void registerAuthRoutes(drogon::HttpAppFramework &app,
                        const std::optional<std::string> &jwtSecret,
                        const std::unordered_set<std::string> &allowedOrigins) {
''',
    "add route preflight support",
)

routes = replace_once(
    routes,
    '''    app.registerHandler("/api/auth/reset-password",
                        [](const drogon::HttpRequestPtr &req,
                           std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
                            handleResetPassword(req, std::move(callback));
                        },
                        {drogon::Post});
}
''',
    '''    app.registerHandler("/api/auth/reset-password",
                        [](const drogon::HttpRequestPtr &req,
                           std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
                            handleResetPassword(req, std::move(callback));
                        },
                        {drogon::Post});

    const auto preflightHandler = [allowedOrigins](
        const drogon::HttpRequestPtr &req,
        std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        callback(buildPreflightResponse(req, allowedOrigins));
    };

    app.registerHandler("/api/auth/validate", preflightHandler, {drogon::Options});
    app.registerHandler("/api/auth/status", preflightHandler, {drogon::Options});
    app.registerHandler("/api/auth/login", preflightHandler, {drogon::Options});
    app.registerHandler("/api/auth/signup", preflightHandler, {drogon::Options});
    app.registerHandler("/api/auth/logout", preflightHandler, {drogon::Options});
    app.registerHandler("/api/auth/verify-email", preflightHandler, {drogon::Options});
    app.registerHandler("/api/auth/resend-verification", preflightHandler, {drogon::Options});
    app.registerHandler("/api/auth/request-password-reset", preflightHandler, {drogon::Options});
    app.registerHandler("/api/auth/reset-password", preflightHandler, {drogon::Options});
}
''',
    "register auth preflight routes",
)
routes_path.write_text(routes, encoding="utf-8")

cmake = cmake_path.read_text(encoding="utf-8")
cmake = replace_once(
    cmake,
    "    src/auth_controller.cpp\n    src/auth_account_store.cpp\n",
    "    src/auth_controller.cpp\n    src/auth_routes.cpp\n    src/auth_account_store.cpp\n",
    "add auth routes production source",
)
cmake_path.write_text(cmake, encoding="utf-8")

print("authentication route extraction applied")
