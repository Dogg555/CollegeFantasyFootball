#!/usr/bin/env python3
"""Structural contracts for ordered application route composition."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MAIN = (ROOT / "backend/src/main.cpp").read_text(encoding="utf-8")
HEADER = (ROOT / "backend/src/app_composition.h").read_text(encoding="utf-8")
COMPOSITION = (ROOT / "backend/src/app_composition.cpp").read_text(encoding="utf-8")
CMAKE = (ROOT / "backend/CMakeLists.txt").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


require('#include "app_composition.h"' in MAIN, "main.cpp must include app_composition.h")
require(
    MAIN.count("cff::app_composition::registerApplicationRoutes(") == 1,
    "main.cpp must delegate route composition exactly once",
)
require(
    MAIN.index("cff::app_composition::registerApplicationRoutes(") < MAIN.index("app.run();"),
    "application routes must be composed before app.run()",
)

route_registrations = (
    "cff::health::registerHealthRoutes(app, jwtSecret, allowedOrigins);",
    "cff::auth::registerAuthRoutes(app, jwtSecret, allowedOrigins);",
    "cff::operations::registerOperationsRoutes(app, jwtSecret, allowedOrigins);",
    "cff::league::registerLeagueRoutes(app, jwtSecret, allowedOrigins);",
    "cff::public_api::registerPublicRoutes(app, allowedOrigins);",
)

for registration in route_registrations:
    require(registration not in MAIN, f"route composition leaked into main.cpp: {registration}")
    require(
        COMPOSITION.count(registration) == 1,
        f"application composition must own exactly one registration: {registration}",
    )

positions = [COMPOSITION.index(registration) for registration in route_registrations]
require(
    positions == sorted(positions),
    "route group order changed: health, auth, operations, league, public must remain stable",
)

for include in (
    '#include "health_routes.h"',
    '#include "auth_routes.h"',
    '#include "operations_routes.h"',
    '#include "league_routes.h"',
    '#include "public_routes.h"',
):
    require(include in COMPOSITION, f"application composition dependency missing: {include}")
    require(include not in MAIN, f"main.cpp still directly depends on a route module: {include}")

require("void registerApplicationRoutes(" in HEADER, "composition interface declaration missing")
require("void registerApplicationRoutes(" in COMPOSITION, "composition implementation missing")
require("app.run();" not in COMPOSITION, "application composition must not own app.run()")
require(
    "cff::server_runtime::" not in COMPOSITION,
    "application composition must not own server runtime configuration",
)
require(
    "cff::ingest_runtime::" not in COMPOSITION,
    "application composition must not own ingestion runtime configuration",
)
require(
    "src/app_composition.cpp" in CMAKE,
    "production target must compile app_composition.cpp",
)

print("application composition boundary contracts passed")
