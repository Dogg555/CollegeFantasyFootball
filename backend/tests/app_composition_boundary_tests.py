#!/usr/bin/env python3
"""Structural contracts for ordered application route composition."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MAIN = (ROOT / "backend/src/main.cpp").read_text(encoding="utf-8")
BOOTSTRAP = (ROOT / "backend/src/application_bootstrap.cpp").read_text(encoding="utf-8")
HEADER = (ROOT / "backend/src/app_composition.h").read_text(encoding="utf-8")
COMPOSITION = (ROOT / "backend/src/app_composition.cpp").read_text(encoding="utf-8")
CMAKE = (ROOT / "backend/CMakeLists.txt").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


require(
    MAIN.count("cff::application::runApplication()") == 1,
    "main.cpp must delegate application startup exactly once",
)
require('#include "app_composition.h"' in BOOTSTRAP, "bootstrap must include app_composition.h")
require(
    BOOTSTRAP.count("cff::app_composition::registerApplicationRoutes(") == 1,
    "bootstrap must delegate route composition exactly once",
)
require(
    BOOTSTRAP.index("cff::app_composition::registerApplicationRoutes(") < BOOTSTRAP.index("app.run();"),
    "application routes must be composed before app.run()",
)

route_registrations = (
    "cff::health::registerHealthRoutes(app, jwtSecret, allowedOrigins);",
    "cff::auth::registerAuthRoutes(app, jwtSecret, allowedOrigins);",
    "cff::operations::registerOperationsRoutes(app, jwtSecret, allowedOrigins);",
    "cff::live_stats::registerLiveStatRoutes(app, jwtSecret, allowedOrigins);",
    "cff::league::registerLeagueRoutes(app, jwtSecret, allowedOrigins);",
    "cff::public_api::registerPublicRoutes(app, allowedOrigins);",
)

for registration in route_registrations:
    require(registration not in MAIN, f"route composition leaked into main.cpp: {registration}")
    require(registration not in BOOTSTRAP, f"route group registration leaked into bootstrap: {registration}")
    require(
        COMPOSITION.count(registration) == 1,
        f"application composition must own exactly one registration: {registration}",
    )

positions = [COMPOSITION.index(registration) for registration in route_registrations]
require(
    positions == sorted(positions),
    "route group order changed: health, auth, operations, live stats, league, public must remain stable",
)

for include in (
    '#include "health_routes.h"',
    '#include "auth_routes.h"',
    '#include "operations_routes.h"',
    '#include "live_stat_routes.h"',
    '#include "league_routes.h"',
    '#include "public_routes.h"',
):
    require(include in COMPOSITION, f"application composition dependency missing: {include}")
    require(include not in MAIN, f"main.cpp directly depends on a route module: {include}")
    require(include not in BOOTSTRAP, f"bootstrap directly depends on a route module: {include}")

require("void registerApplicationRoutes(" in HEADER, "composition interface declaration missing")
require("void registerApplicationRoutes(" in COMPOSITION, "composition implementation missing")
require("app.run();" not in COMPOSITION, "application composition must not own app.run()")
require("app.run();" in BOOTSTRAP, "application bootstrap must own app.run()")
require(
    "cff::server_runtime::" not in COMPOSITION,
    "application composition must not own server runtime configuration",
)
require(
    "cff::ingest_runtime::" not in COMPOSITION,
    "application composition must not own ingestion runtime configuration",
)
require("src/app_composition.cpp" in CMAKE, "production target must compile app_composition.cpp")
require("src/application_bootstrap.cpp" in CMAKE, "targets must compile application_bootstrap.cpp")
require("src/live_stat_routes.cpp" in CMAKE, "production target must compile live_stat_routes.cpp")

print("application composition boundary contracts passed")
