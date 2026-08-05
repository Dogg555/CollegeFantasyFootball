#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[2]
main = (root / "backend/src/main.cpp").read_text(encoding="utf-8")
composition = (root / "backend/src/app_composition.cpp").read_text(encoding="utf-8")
routes = (root / "backend/src/auth_routes.cpp").read_text(encoding="utf-8")
header = (root / "backend/src/auth_routes.h").read_text(encoding="utf-8")
security = (root / "backend/src/http_security.cpp").read_text(encoding="utf-8")
cmake = (root / "backend/CMakeLists.txt").read_text(encoding="utf-8")

route_paths = (
    "/api/auth/status",
    "/api/auth/validate",
    "/api/auth/signup",
    "/api/auth/login",
    "/api/auth/logout",
    "/api/auth/verify-email",
    "/api/auth/resend-verification",
    "/api/auth/request-password-reset",
    "/api/auth/reset-password",
)

if "/api/auth/" in main:
    raise SystemExit("authentication route registration leaked back into main.cpp")
if "cff::auth::registerAuthRoutes(" in main:
    raise SystemExit("main.cpp must delegate authentication registration through app composition")
if composition.count("cff::auth::registerAuthRoutes(app, jwtSecret, allowedOrigins);") != 1:
    raise SystemExit("application composition must register authentication routes exactly once")
if routes.count("void registerAuthRoutes(") != 1:
    raise SystemExit("auth_routes.cpp must define registerAuthRoutes exactly once")
if header.count("void registerAuthRoutes(") != 1:
    raise SystemExit("auth_routes.h must declare registerAuthRoutes exactly once")

for path in route_paths:
    if routes.count(f'"{path}"') != 2:
        raise SystemExit(f"auth route must have one normal and one OPTIONS registration: {path}")

required_route_contracts = (
    'error["valid"] = false',
    'error["unavailable"] = true',
    'payload["valid"] = authorized',
    'payload["signupEnabled"] = dbReady',
    'payload["loginEnabled"] = dbReady',
    'payload["emailFlowsEnabled"] = emailDeliveryReady()',
    'cff::http::bearerToken(req)',
    'cff::http::isAuthorized(req, jwtSecret)',
    'cff::http::buildPreflightResponse(req, allowedOrigins)',
)
for contract in required_route_contracts:
    if contract not in routes:
        raise SystemExit(f"auth route contract missing: {contract}")

shared_preflight_contracts = (
    'resp->setStatusCode(drogon::k204NoContent)',
    'resp->addHeader("Access-Control-Allow-Headers", "Authorization, Content-Type, X-Request-ID, Idempotency-Key")',
    'resp->addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, PATCH, DELETE, OPTIONS")',
)
for contract in shared_preflight_contracts:
    if contract not in security:
        raise SystemExit(f"shared preflight contract missing: {contract}")

if routes.count("{drogon::Options}") != len(route_paths):
    raise SystemExit("every authentication endpoint must retain an OPTIONS route")
if "src/auth_routes.cpp" not in cmake:
    raise SystemExit("auth_routes.cpp is not part of the production target")
if "src/http_security.cpp" not in cmake:
    raise SystemExit("http_security.cpp is not part of the production target")
if "src/app_composition.cpp" not in cmake:
    raise SystemExit("app_composition.cpp is not part of the production target")

print("authentication route boundary contracts passed")
