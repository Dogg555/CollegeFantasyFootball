#!/usr/bin/env python3
"""Structural contracts for the application bootstrap boundary."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MAIN = (ROOT / "backend/src/main.cpp").read_text(encoding="utf-8")
HEADER = (ROOT / "backend/src/application_bootstrap.h").read_text(encoding="utf-8")
BOOTSTRAP = (ROOT / "backend/src/application_bootstrap.cpp").read_text(encoding="utf-8")
CMAKE = (ROOT / "backend/CMakeLists.txt").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


require('#include "application_bootstrap.h"' in MAIN, "main.cpp must include application_bootstrap.h")
require(
    MAIN.count("cff::application::runApplication()") == 1,
    "main.cpp must delegate to runApplication exactly once",
)
require(
    "return cff::application::runApplication();" in MAIN,
    "main.cpp must return the application exit code",
)

for forbidden in (
    "DROGON_FOUND",
    "drogon::app()",
    "loadRuntimeConfig",
    "configureSecurityAndCors",
    "configureCfbdIngest",
    "configureLiveStatWorker",
    "configureListener",
    "registerApplicationRoutes",
    "app.run()",
    "College Fantasy Football backend scaffold",
):
    require(forbidden not in MAIN, f"main.cpp still owns bootstrap detail: {forbidden}")

require("int runApplication();" in HEADER, "application bootstrap interface missing")
require("int runApplication()" in BOOTSTRAP, "application bootstrap implementation missing")
require("#ifdef DROGON_FOUND" in BOOTSTRAP, "bootstrap must retain Drogon and stub paths")
require(
    '#include "live_stat_worker.h"' in BOOTSTRAP,
    "bootstrap must include the live stat worker interface",
)

ordered_steps = (
    "drogon::app()",
    "cff::config::loadRuntimeConfig()",
    "cff::server_runtime::configureSecurityAndCors(",
    "cff::ingest_runtime::configureCfbdIngest(",
    "cff::live_stats::configureLiveStatWorker();",
    "cff::server_runtime::configureListener(",
    "cff::app_composition::registerApplicationRoutes(",
    "app.run();",
)
positions = [BOOTSTRAP.index(step) for step in ordered_steps]
require(
    positions == sorted(positions),
    "application startup order changed",
)
require(
    BOOTSTRAP.count("cff::live_stats::configureLiveStatWorker();") == 1,
    "live stat worker must be configured exactly once",
)

for stub_line in (
    "College Fantasy Football backend scaffold (Drogon not linked).",
    "Build with -DDROGON_FOUND=ON and link Drogon to run the HTTP server.",
):
    require(stub_line in BOOTSTRAP, f"stub output changed: {stub_line}")

require("return 0;" in BOOTSTRAP, "bootstrap exit status changed")
require(
    CMAKE.count("src/application_bootstrap.cpp") == 2,
    "application_bootstrap.cpp must compile in stub and production targets",
)
require("src/main.cpp" in CMAKE, "main.cpp must remain the executable entry point")
require("src/live_stat_worker.cpp" in CMAKE, "production target must compile live_stat_worker.cpp")

print("application bootstrap boundary contracts passed")
