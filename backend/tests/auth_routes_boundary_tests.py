#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[2]
main = (root / "backend/src/main.cpp").read_text(encoding="utf-8")
routes = (root / "backend/src/auth_routes.cpp").read_text(encoding="utf-8")
header = (root / "backend/src/auth_routes.h").read_text(encoding="utf-8")
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
if main.count("cff::auth::registerAuthRoutes(app, jwtSecret);") != 1:
    raise SystemExit("main.cpp must register authentication routes exactly once")
if routes.count("void registerAuthRoutes(") != 1:
    raise SystemExit("auth_routes.cpp must define registerAuthRoutes exactly once")
if header.count("void registerAuthRoutes(") != 1:
    raise SystemExit("auth_routes.h must declare registerAuthRoutes exactly once")

for path in route_paths:
    if routes.count(f'"{path}"') != 1:
        raise SystemExit(f"auth route must be registered exactly once: {path}")

required_route_contracts = (
    'error["valid"] = false',
    'error["unavailable"] = true',
    'payload["valid"] = authorized',
    'payload["signupEnabled"] = dbReady',
    'payload["loginEnabled"] = dbReady',
    'payload["emailFlowsEnabled"] = emailDeliveryReady()',
)
for contract in required_route_contracts:
    if contract not in routes:
        raise SystemExit(f"auth route contract missing: {contract}")

if "src/auth_routes.cpp" not in cmake:
    raise SystemExit("auth_routes.cpp is not part of the production target")

print("authentication route boundary contracts passed")
