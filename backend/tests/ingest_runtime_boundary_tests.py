#!/usr/bin/env python3
"""Structural contracts for the CFBD ingestion runtime boundary."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MAIN = (ROOT / "backend/src/main.cpp").read_text(encoding="utf-8")
HEADER = (ROOT / "backend/src/ingest_runtime.h").read_text(encoding="utf-8")
IMPLEMENTATION = (ROOT / "backend/src/ingest_runtime.cpp").read_text(encoding="utf-8")
CMAKE = (ROOT / "backend/CMakeLists.txt").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


require('#include "ingest_runtime.h"' in MAIN, "main.cpp must include the ingestion runtime")
require(
    MAIN.count("cff::ingest_runtime::configureCfbdIngest(") == 1,
    "main.cpp must delegate ingestion lifecycle exactly once",
)
require(
    "cff::runCfbdIngestOnce" in MAIN,
    "main.cpp must inject the existing one-shot ingestion function",
)

for forbidden in (
    "void logIngestResult(",
    "void startBackgroundCfbdIngest(",
    "std::this_thread::sleep_for",
    "[cfbd] background ingest",
    "[cfbd] CFBD_INGEST_ON_STARTUP",
    "while (true)",
):
    require(forbidden not in MAIN, f"main.cpp still owns ingestion runtime detail: {forbidden}")

for contract in (
    "using IngestRunner",
    "runStartupIngest",
    "runScheduledIngestCycle",
    "configureCfbdIngest",
):
    require(contract in HEADER, f"ingestion runtime interface missing {contract}")

for owned_detail in (
    "void logIngestResult(",
    "std::thread(",
    "std::this_thread::sleep_for",
    "[cfbd] background ingest enabled every ",
    "[cfbd] background ingest starting...",
    "[cfbd] CFBD_INGEST_ON_STARTUP enabled; starting ingest...",
    "while (true)",
):
    require(owned_detail in IMPLEMENTATION, f"ingestion runtime does not own {owned_detail}")

require(
    "src/ingest_runtime.cpp" in CMAKE,
    "production target must compile the ingestion runtime module",
)
require(
    "tests/ingest_runtime_tests.cpp" in CMAKE,
    "CTest must register deterministic ingestion runtime tests",
)

print("ingestion runtime boundary contracts passed")
