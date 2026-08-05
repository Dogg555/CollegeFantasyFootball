#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[2]
main = (root / "backend/src/main.cpp").read_text(encoding="utf-8")
composition = (root / "backend/src/app_composition.cpp").read_text(encoding="utf-8")
routes = (root / "backend/src/health_routes.cpp").read_text(encoding="utf-8")
header = (root / "backend/src/health_routes.h").read_text(encoding="utf-8")
status_advice = (root / "backend/src/health_status.cpp").read_text(encoding="utf-8")
cmake = (root / "backend/CMakeLists.txt").read_text(encoding="utf-8")

for implementation in (
    "Json::Value healthPayload(",
    "drogon::HttpStatusCode healthStatusCode(",
    "const auto healthHandler =",
):
    if implementation in main:
        raise SystemExit(f"health implementation leaked into main.cpp: {implementation}")

if '.registerHandler("/health"' in main:
    raise SystemExit("/health registration leaked into main.cpp")
if '.registerHandler("/api/health"' in main:
    raise SystemExit("/api/health registration leaked into main.cpp")
if "cff::health::registerHealthRoutes(" in main:
    raise SystemExit("main.cpp must delegate health registration through app composition")
if composition.count(
    "cff::health::registerHealthRoutes(app, jwtSecret, allowedOrigins);"
) != 1:
    raise SystemExit("application composition must register health routes exactly once")

if routes.count('"/health"') != 1:
    raise SystemExit("health module must register one GET /health route")
if routes.count('"/api/health"') != 2:
    raise SystemExit("health module must register GET and OPTIONS /api/health routes")
if routes.count("{drogon::Get}") != 2:
    raise SystemExit("health module must retain two GET registrations")
if routes.count("{drogon::Options}") != 1:
    raise SystemExit("API health must retain one OPTIONS registration")

for declaration in (
    "Json::Value buildHealthPayload(",
    "drogon::HttpStatusCode healthStatusCode(",
    "void registerHealthRoutes(",
):
    if declaration not in header:
        raise SystemExit(f"health_routes.h declaration missing: {declaration}")
    if declaration not in routes:
        raise SystemExit(f"health_routes.cpp implementation missing: {declaration}")

for contract in (
    'payload["status"] = "ok"',
    'payload["service"] = "college-ff-api"',
    'payload["jwtSecretConfigured"] = jwtSecret.has_value()',
    'payload["allowedOriginsConfigured"] = !allowedOrigins.empty()',
    'payload["persistentDbRequired"]',
    'payload["emailDeliveryConfigured"]',
    'payload["emailVerificationRequired"]',
    'payload["passwordPolicy"]["minLength"]',
    'payload["passwordPolicy"]["maxLength"]',
    'payload["databaseConfigured"]',
    'payload["database"] = conn ? "ok" : "unavailable"',
    'payload["database"] = "not_configured"',
    'payload["database"] = "not_compiled"',
    'payload["status"] = "degraded"',
    'return drogon::k503ServiceUnavailable',
    'return drogon::k200OK',
    'cff::http::buildPreflightResponse(',
):
    if contract not in routes:
        raise SystemExit(f"health behavior contract missing: {contract}")

for retained_advice in (
    "enforceTruthfulHealthStatus",
    'path == "/health" || path == "/api/health"',
    "response->setStatusCode(drogon::k503ServiceUnavailable)",
):
    if retained_advice not in status_advice:
        raise SystemExit(f"truthful health-status advice missing: {retained_advice}")

if "src/health_routes.cpp" not in cmake:
    raise SystemExit("health_routes.cpp is not part of the production target")
if "src/app_composition.cpp" not in cmake:
    raise SystemExit("app_composition.cpp is not part of the production target")

print("health route boundary contracts passed")
