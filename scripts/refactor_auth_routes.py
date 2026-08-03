#!/usr/bin/env python3
"""Finish moving authentication preflight registration into auth_routes.cpp."""

from pathlib import Path

root = Path(__file__).resolve().parents[1]
main_path = root / "backend/src/main.cpp"
routes_path = root / "backend/src/auth_routes.cpp"


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected one source anchor, found {count}")
    return text.replace(old, new, 1)


main = main_path.read_text(encoding="utf-8")
if main.count("cff::auth::registerAuthRoutes(app, jwtSecret, allowedOrigins);") != 1:
    raise RuntimeError("main.cpp must already delegate authentication registration exactly once")

preflight_secure = '        .registerHandler("/api/secure/ping", preflightHandler, {drogon::Options})\n'
auth_preflight_start = '        .registerHandler("/api/auth/validate", preflightHandler, {drogon::Options})\n'
league_preflight_start = '        .registerHandler("/api/leagues", preflightHandler, {drogon::Options})\n'
secure_index = main.find(preflight_secure)
auth_index = main.find(auth_preflight_start, secure_index)
league_index = main.find(league_preflight_start, auth_index)
if secure_index < 0 or auth_index < 0 or league_index < 0:
    raise RuntimeError("authentication preflight route block anchors not found")
main = main[:auth_index] + main[league_index:]
if "/api/auth/" in main:
    raise RuntimeError("main.cpp still contains authentication route wiring")
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

for path in (
    "/api/auth/validate",
    "/api/auth/status",
    "/api/auth/login",
    "/api/auth/signup",
    "/api/auth/logout",
    "/api/auth/verify-email",
    "/api/auth/resend-verification",
    "/api/auth/request-password-reset",
    "/api/auth/reset-password",
):
    if routes.count(f'"{path}"') != 2:
        raise RuntimeError(f"expected normal and OPTIONS registration for {path}")

routes_path.write_text(routes, encoding="utf-8")
print("authentication route preflight extraction applied")
