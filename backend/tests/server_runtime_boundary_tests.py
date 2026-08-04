#!/usr/bin/env python3
"""Structural contracts for HTTP server runtime ownership and startup order."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MAIN = (ROOT / "backend/src/main.cpp").read_text(encoding="utf-8")
BOOTSTRAP = (ROOT / "backend/src/application_bootstrap.cpp").read_text(encoding="utf-8")
COMPOSITION = (ROOT / "backend/src/app_composition.cpp").read_text(encoding="utf-8")
HEADER = (ROOT / "backend/src/server_runtime.h").read_text(encoding="utf-8")
IMPLEMENTATION = (ROOT / "backend/src/server_runtime.cpp").read_text(encoding="utf-8")
CMAKE = (ROOT / "backend/CMakeLists.txt").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


require(
    MAIN.count("cff::application::runApplication()") == 1,
    "main.cpp must delegate application startup exactly once",
)
require('#include "server_runtime.h"' in BOOTSTRAP, "bootstrap must include server_runtime.h")
require(
    BOOTSTRAP.count("cff::server_runtime::configureSecurityAndCors(") == 1,
    "bootstrap must delegate security and CORS configuration exactly once",
)
require(
    BOOTSTRAP.count("cff::server_runtime::configureListener(") == 1,
    "bootstrap must delegate listener configuration exactly once",
)

for forbidden in (
    "app.setSSLFiles(",
    "app.registerPostHandlingAdvice(",
    "cff::http::applyCorsHeaders(",
    "app.addListener(",
    ".setThreadNum(",
    "std::thread::hardware_concurrency()",
    "JWT_SECRET is not set; secure endpoints will reject all requests.",
    "SSL enabled with provided certificate and key.",
    "SSL not configured. For testing only.",
    "ALLOWED_ORIGINS not set; cross-origin requests will be blocked.",
):
    require(forbidden not in MAIN, f"main.cpp still owns server runtime detail: {forbidden}")
    require(forbidden not in BOOTSTRAP, f"bootstrap contains server runtime implementation: {forbidden}")

for interface_contract in (
    "bool configureSecurityAndCors(",
    "void configureListener(",
    "const cff::config::RuntimeConfig &runtimeConfig",
):
    require(interface_contract in HEADER, f"server runtime interface missing {interface_contract}")

for owned_detail in (
    "app.setSSLFiles(",
    "app.registerPostHandlingAdvice(",
    "cff::http::applyCorsHeaders(",
    "app.addListener(\"0.0.0.0\"",
    ".setThreadNum(std::thread::hardware_concurrency())",
    "JWT_SECRET is not set; secure endpoints will reject all requests.",
    "SSL enabled with provided certificate and key.",
    "SSL not configured. For testing only.",
    "ALLOWED_ORIGINS not set; cross-origin requests will be blocked.",
):
    require(owned_detail in IMPLEMENTATION, f"server runtime does not own {owned_detail}")

security_index = BOOTSTRAP.index("cff::server_runtime::configureSecurityAndCors(")
ingest_index = BOOTSTRAP.index("cff::ingest_runtime::configureCfbdIngest(")
listener_index = BOOTSTRAP.index("cff::server_runtime::configureListener(")
composition_index = BOOTSTRAP.index("cff::app_composition::registerApplicationRoutes(")
run_index = BOOTSTRAP.index("app.run();")
require(
    security_index < ingest_index < listener_index < composition_index < run_index,
    "startup ordering changed: security, startup ingestion, listener, routes, and run must remain ordered",
)

for route_registration in (
    "cff::health::registerHealthRoutes(",
    "cff::auth::registerAuthRoutes(",
    "cff::operations::registerOperationsRoutes(",
    "cff::league::registerLeagueRoutes(",
    "cff::public_api::registerPublicRoutes(",
):
    require(route_registration not in MAIN, f"main.cpp owns route composition: {route_registration}")
    require(route_registration not in BOOTSTRAP, f"bootstrap owns a route group directly: {route_registration}")
    require(route_registration in COMPOSITION, f"application composition missing route group: {route_registration}")
    require(route_registration not in IMPLEMENTATION, f"server runtime must not own routes: {route_registration}")

require("app.run();" not in MAIN, "main.cpp must delegate the run boundary")
require("app.run();" in BOOTSTRAP, "application bootstrap must own app.run()")
require("app.run();" not in IMPLEMENTATION, "server runtime must not own app.run()")
require("app.run();" not in COMPOSITION, "application composition must not own app.run()")
require("src/server_runtime.cpp" in CMAKE, "production target must compile server_runtime.cpp")
require("src/app_composition.cpp" in CMAKE, "production target must compile app_composition.cpp")
require("src/application_bootstrap.cpp" in CMAKE, "targets must compile application_bootstrap.cpp")

print("server runtime boundary contracts passed")
