#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[2]
main = (root / "backend/src/main.cpp").read_text(encoding="utf-8")
bootstrap = (root / "backend/src/application_bootstrap.cpp").read_text(encoding="utf-8")
composition = (root / "backend/src/app_composition.cpp").read_text(encoding="utf-8")
server_runtime = (root / "backend/src/server_runtime.cpp").read_text(encoding="utf-8")
auth_routes = (root / "backend/src/auth_routes.cpp").read_text(encoding="utf-8")
operations_routes = (root / "backend/src/operations_routes.cpp").read_text(encoding="utf-8")
league_routes = (root / "backend/src/league_routes.cpp").read_text(encoding="utf-8")
public_routes = (root / "backend/src/public_routes.cpp").read_text(encoding="utf-8")
security = (root / "backend/src/http_security.cpp").read_text(encoding="utf-8")
header = (root / "backend/src/http_security.h").read_text(encoding="utf-8")
cmake = (root / "backend/CMakeLists.txt").read_text(encoding="utf-8")

owned_functions = (
    "bearerToken",
    "isAuthorized",
    "accountEmailForRequest",
    "requireAccount",
    "isAdminRequest",
    "requireAdmin",
    "applyCorsHeaders",
    "withConfiguredCorsHeaders",
    "buildPreflightResponse",
)

for function in owned_functions:
    if f"{function}(" not in security:
        raise SystemExit(f"http_security.cpp must own {function}")
    if f"{function}(" not in header:
        raise SystemExit(f"http_security.h must declare {function}")

for implementation in (
    "bool isAuthorized(",
    "std::optional<std::string> accountEmailForRequest(",
    "bool requireAccount(",
    "bool isAdminRequest(",
    "bool requireAdmin(",
    "void applyCorsHeaders(",
    "drogon::HttpResponsePtr withConfiguredCorsHeaders(",
    "drogon::HttpResponsePtr buildPreflightResponse(",
):
    for owner_name, source in (
        ("main.cpp", main),
        ("application_bootstrap.cpp", bootstrap),
        ("app_composition.cpp", composition),
        ("server_runtime.cpp", server_runtime),
        ("auth_routes.cpp", auth_routes),
        ("operations_routes.cpp", operations_routes),
        ("league_routes.cpp", league_routes),
        ("public_routes.cpp", public_routes),
    ):
        if implementation in source:
            raise SystemExit(f"shared HTTP security implementation leaked into {owner_name}: {implementation}")

if "cff::application::runApplication()" not in main:
    raise SystemExit("main.cpp must delegate to the application bootstrap")

required_bootstrap_delegation = (
    "cff::server_runtime::configureSecurityAndCors(",
    "cff::app_composition::registerApplicationRoutes(",
)
for delegation in required_bootstrap_delegation:
    if delegation not in bootstrap:
        raise SystemExit(f"application bootstrap HTTP security delegation missing: {delegation}")

if "cff::public_api::registerPublicRoutes(" in main or "cff::public_api::registerPublicRoutes(" in bootstrap:
    raise SystemExit("entry-point layers must not directly own public route composition")
if "cff::public_api::registerPublicRoutes(app, allowedOrigins)" not in composition:
    raise SystemExit("app_composition.cpp must register the public route group")

required_server_runtime_delegation = (
    "app.registerPostHandlingAdvice(",
    "cff::http::applyCorsHeaders(request, response, allowedOrigins)",
)
for delegation in required_server_runtime_delegation:
    if delegation not in server_runtime:
        raise SystemExit(f"server_runtime.cpp HTTP security delegation missing: {delegation}")

required_auth_route_delegation = (
    "cff::http::bearerToken(req)",
    "cff::http::isAuthorized(req, jwtSecret)",
    "cff::http::buildPreflightResponse(req, allowedOrigins)",
)
for delegation in required_auth_route_delegation:
    if delegation not in auth_routes:
        raise SystemExit(f"auth_routes.cpp HTTP security delegation missing: {delegation}")

required_operations_delegation = (
    "cff::http::isAuthorized(request, jwtSecret)",
    "cff::http::requireAdmin(",
    "cff::http::buildPreflightResponse(request, allowedOrigins)",
)
for delegation in required_operations_delegation:
    if delegation not in operations_routes:
        raise SystemExit(f"operations_routes.cpp HTTP security delegation missing: {delegation}")

required_league_route_delegation = (
    "cff::http::requireAccount(req, callback, jwtSecret, accountEmail)",
    "cff::http::buildPreflightResponse(request, allowedOrigins)",
)
for delegation in required_league_route_delegation:
    if delegation not in league_routes:
        raise SystemExit(f"league_routes.cpp HTTP security delegation missing: {delegation}")

if "cff::http::buildPreflightResponse(request, allowedOrigins)" not in public_routes:
    raise SystemExit("public_routes.cpp HTTP security delegation missing")

required_behavior = (
    'error["error"] = "Unauthorized"',
    'error["error"] = "Admin access required"',
    'resp->setStatusCode(drogon::k401Unauthorized)',
    'resp->setStatusCode(drogon::k403Forbidden)',
    'resp->addHeader("Access-Control-Allow-Origin", origin)',
    'resp->addHeader("Vary", "Origin")',
    'resp->addHeader("Access-Control-Allow-Headers", "Authorization, Content-Type, Idempotency-Key, X-Request-ID")',
    'resp->addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS")',
    'applyCorsHeaders(req, resp, runtimeConfig.allowedOrigins)',
    'resp->setStatusCode(drogon::k204NoContent)',
    'return std::string{"admin@example.com"}',
    'adminIdentity = "ops-token"',
)
for contract in required_behavior:
    if contract not in security:
        raise SystemExit(f"HTTP security behavior contract missing: {contract}")

for source_path in (
    "src/http_security.cpp",
    "src/server_runtime.cpp",
    "src/app_composition.cpp",
    "src/application_bootstrap.cpp",
):
    if source_path not in cmake:
        raise SystemExit(f"{source_path} is not part of the server targets")

print("HTTP security boundary contracts passed")
