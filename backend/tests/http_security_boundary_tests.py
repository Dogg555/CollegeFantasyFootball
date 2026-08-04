#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[2]
main = (root / "backend/src/main.cpp").read_text(encoding="utf-8")
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
    "drogon::HttpResponsePtr buildPreflightResponse(",
):
    if implementation in main:
        raise SystemExit(f"shared HTTP security implementation leaked into main.cpp: {implementation}")
    if implementation in server_runtime:
        raise SystemExit(f"shared HTTP security implementation leaked into server_runtime.cpp: {implementation}")
    if implementation in auth_routes:
        raise SystemExit(f"shared HTTP security implementation leaked into auth_routes.cpp: {implementation}")
    if implementation in operations_routes:
        raise SystemExit(f"shared HTTP security implementation leaked into operations_routes.cpp: {implementation}")
    if implementation in league_routes:
        raise SystemExit(f"shared HTTP security implementation leaked into league_routes.cpp: {implementation}")
    if implementation in public_routes:
        raise SystemExit(f"shared HTTP security implementation leaked into public_routes.cpp: {implementation}")

required_main_delegation = (
    "cff::server_runtime::configureSecurityAndCors(",
    "cff::public_api::registerPublicRoutes(app, allowedOrigins)",
)
for delegation in required_main_delegation:
    if delegation not in main:
        raise SystemExit(f"main.cpp HTTP security delegation missing: {delegation}")

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

required_public_route_delegation = (
    "cff::http::buildPreflightResponse(request, allowedOrigins)",
)
for delegation in required_public_route_delegation:
    if delegation not in public_routes:
        raise SystemExit(f"public_routes.cpp HTTP security delegation missing: {delegation}")

required_behavior = (
    'error["error"] = "Unauthorized"',
    'error["error"] = "Admin access required"',
    'resp->setStatusCode(drogon::k401Unauthorized)',
    'resp->setStatusCode(drogon::k403Forbidden)',
    'resp->addHeader("Access-Control-Allow-Origin", origin)',
    'resp->addHeader("Vary", "Origin")',
    'resp->addHeader("Access-Control-Allow-Headers", "Authorization, Content-Type")',
    'resp->addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS")',
    'resp->setStatusCode(drogon::k204NoContent)',
    'return std::string{"admin@example.com"}',
    'adminIdentity = "ops-token"',
)
for contract in required_behavior:
    if contract not in security:
        raise SystemExit(f"HTTP security behavior contract missing: {contract}")

if "src/http_security.cpp" not in cmake:
    raise SystemExit("http_security.cpp is not part of the production target")
if "src/server_runtime.cpp" not in cmake:
    raise SystemExit("server_runtime.cpp is not part of the production target")

print("HTTP security boundary contracts passed")
