#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[2]
main = (root / "backend/src/main.cpp").read_text(encoding="utf-8")
composition = (root / "backend/src/app_composition.cpp").read_text(encoding="utf-8")
routes = (root / "backend/src/operations_routes.cpp").read_text(encoding="utf-8")
header = (root / "backend/src/operations_routes.h").read_text(encoding="utf-8")
cmake = (root / "backend/CMakeLists.txt").read_text(encoding="utf-8")

route_paths = (
    "/api/secure/ping",
    "/api/admin/ingest/cfbd",
    "/api/admin/ingest/cfbd/status",
    "/api/admin/ingest/cfbd/live",
    "/api/admin/ingest/cfbd/live/status",
)

for path in route_paths:
    if f'"{path}"' in main:
        raise SystemExit(f"operations route leaked into main.cpp: {path}")
    if routes.count(f'"{path}"') != 2:
        raise SystemExit(
            f"operations route must retain one normal and one OPTIONS registration: {path}"
        )

if "Json::Value dbIngestionStatus()" in main:
    raise SystemExit("ingestion status persistence leaked into main.cpp")
if "struct PgConnDeleter" in main:
    raise SystemExit("operations PostgreSQL connection ownership leaked into main.cpp")
if "cff::operations::registerOperationsRoutes(" in main:
    raise SystemExit("main.cpp must delegate operations registration through app composition")
if composition.count(
    "cff::operations::registerOperationsRoutes(app, jwtSecret, allowedOrigins);"
) != 1:
    raise SystemExit("application composition must register operations routes exactly once")

for declaration in (
    "void registerOperationsRoutes(",
):
    if declaration not in header:
        raise SystemExit(f"operations header declaration missing: {declaration}")
    if declaration not in routes:
        raise SystemExit(f"operations implementation missing: {declaration}")

required_contracts = (
    "cff::http::isAuthorized(request, jwtSecret)",
    'response->setBody("unauthorized")',
    'response->setBody(R"({"status":"ok","scope":"secure"})")',
    "cff::http::requireAdmin(",
    "cff::runCfbdIngestOnce()",
    "cff::runLiveScoreIngestOnce()",
    "cff::liveScoreIngestStatus()",
    'payload["status"] =',
    'payload["ingested"]',
    'payload["updated"]',
    'payload["games"]',
    'payload["liveGames"]',
    'payload["apiCalls"]',
    'payload["fullRosterSchedule"] = "weekly"',
    'payload["manualTriggerAvailable"] = true',
    'payload["status"] = "unavailable"',
    'payload["status"] = "ok"',
    '"FROM ingestion_runs ORDER BY started_at DESC LIMIT 10"',
    "cff::http::buildPreflightResponse(request, allowedOrigins)",
)
for contract in required_contracts:
    if contract not in routes:
        raise SystemExit(f"operations behavior contract missing: {contract}")

if routes.count("{drogon::Options}") != len(route_paths):
    raise SystemExit("every operations endpoint must retain an OPTIONS route")
if "src/operations_routes.cpp" not in cmake:
    raise SystemExit("operations_routes.cpp is not part of the production target")
if "src/app_composition.cpp" not in cmake:
    raise SystemExit("app_composition.cpp is not part of the production target")

print("operations route boundary contracts passed")
