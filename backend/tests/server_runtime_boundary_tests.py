#!/usr/bin/env python3
"""Structural contracts for HTTP server runtime ownership and startup order."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MAIN = (ROOT / "backend/src/main.cpp").read_text(encoding="utf-8")
HEADER = (ROOT / "backend/src/server_runtime.h").read_text(encoding="utf-8")
IMPLEMENTATION = (ROOT / "backend/src/server_runtime.cpp").read_text(encoding="utf-8")
CMAKE = (ROOT / "backend/CMakeLists.txt").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


require('#include "server_runtime.h"' in MAIN, "main.cpp must include server_runtime.h")
require(
    MAIN.count("cff::server_runtime::configureSecurityAndCors(") == 1,
    "main.cpp must delegate security and CORS configuration exactly once",
)
require(
    MAIN.count("cff::server_runtime::configureListener(") == 1,
    "main.cpp must delegate listener configuration exactly once",
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

security_index = MAIN.index("cff::server_runtime::configureSecurityAndCors(")
ingest_index = MAIN.index("cff::ingest_runtime::configureCfbdIngest(")
listener_index = MAIN.index("cff::server_runtime::configureListener(")
health_index = MAIN.index("cff::health::registerHealthRoutes(")
run_index = MAIN.index("app.run();")
require(
    security_index < ingest_index < listener_index < health_index < run_index,
    "startup ordering changed: security, startup ingestion, listener, routes, and run must remain ordered",
)

for route_registration in (
    "cff::health::registerHealthRoutes(",
    "cff::auth::registerAuthRoutes(",
    "cff::operations::registerOperationsRoutes(",
    "cff::league::registerLeagueRoutes(",
    "cff::public_api::registerPublicRoutes(",
):
    require(route_registration in MAIN, f"main.cpp must retain route composition: {route_registration}")
    require(route_registration not in IMPLEMENTATION, f"server runtime must not own routes: {route_registration}")

require("app.run();" in MAIN, "main.cpp must retain the application run boundary")
require("app.run();" not in IMPLEMENTATION, "server runtime must not own app.run in this task")
require("src/server_runtime.cpp" in CMAKE, "production target must compile server_runtime.cpp")

print("server runtime boundary contracts passed")
