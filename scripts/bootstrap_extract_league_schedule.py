#!/usr/bin/env python3
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
HANDLER_PATH = ROOT / "backend/src/handlers/league_handler.cpp"
CMAKE_PATH = ROOT / "backend/CMakeLists.txt"


def remove_function(source: str, signature: str) -> str:
    start = source.find(signature)
    if start < 0:
        raise RuntimeError(f"function signature not found: {signature}")
    brace = source.find("{", start + len(signature))
    if brace < 0:
        raise RuntimeError(f"function body not found: {signature}")
    depth = 0
    end = brace
    while end < len(source):
        char = source[end]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                end += 1
                while end < len(source) and source[end] in " \t":
                    end += 1
                if source.startswith("\r\n", end):
                    end += 2
                elif end < len(source) and source[end] == "\n":
                    end += 1
                if source.startswith("\r\n", end):
                    end += 2
                elif end < len(source) and source[end] == "\n":
                    end += 1
                return source[:start] + source[end:]
        end += 1
    raise RuntimeError(f"unterminated function body: {signature}")


handler = HANDLER_PATH.read_text(encoding="utf-8")
include_anchor = '#include "../league_models.h"\n'
if '#include "../league_schedule.h"' not in handler:
    if include_anchor not in handler:
        raise RuntimeError("league_models include anchor missing")
    handler = handler.replace(
        include_anchor,
        include_anchor + '#include "../league_schedule.h"\n',
        1,
    )

for signature in (
    "Json::Value activeMembers(const Json::Value &members)",
    "Json::Value buildMatchups(const Json::Value &members,",
    "Json::Value buildSeasonSchedule(const Json::Value &members,",
    "std::string currentDraftManager(const Json::Value &draftOrder,",
):
    if signature in handler:
        handler = remove_function(handler, signature)

for name in ("buildMatchups", "buildSeasonSchedule", "currentDraftManager"):
    handler = re.sub(
        rf"(?<![A-Za-z0-9_:]){name}\(",
        f"cff::league_schedule::{name}(",
        handler,
    )

for forbidden in (
    "Json::Value activeMembers(const Json::Value &members)",
    "Json::Value buildMatchups(const Json::Value &members,",
    "Json::Value buildSeasonSchedule(const Json::Value &members,",
    "std::string currentDraftManager(const Json::Value &draftOrder,",
):
    if forbidden in handler:
        raise RuntimeError(f"league schedule implementation remains in handler: {forbidden}")

for required in (
    "cff::league_schedule::buildMatchups(",
    "cff::league_schedule::buildSeasonSchedule(",
    "cff::league_schedule::currentDraftManager(",
):
    if required not in handler:
        raise RuntimeError(f"league schedule delegation missing: {required}")

HANDLER_PATH.write_text(handler, encoding="utf-8")

cmake = CMAKE_PATH.read_text(encoding="utf-8")
if "    src/league_schedule.cpp\n" not in cmake:
    source_anchor = "    src/league_models.cpp\n"
    if source_anchor not in cmake:
        raise RuntimeError("league_models source anchor missing")
    cmake = cmake.replace(
        source_anchor,
        source_anchor + "    src/league_schedule.cpp\n",
        1,
    )

if "add_executable(league_schedule_tests" not in cmake:
    test_anchor = "    add_test(NAME ingest_runtime_tests COMMAND ingest_runtime_tests)\n"
    if test_anchor not in cmake:
        raise RuntimeError("ingest runtime test anchor missing")
    test_block = """

    add_executable(league_schedule_tests
        tests/league_schedule_tests.cpp
        src/league_schedule.cpp
    )
    target_include_directories(league_schedule_tests PRIVATE src)
    target_link_libraries(league_schedule_tests PRIVATE Drogon::Drogon)
    add_test(NAME league_schedule_tests COMMAND league_schedule_tests)
"""
    cmake = cmake.replace(test_anchor, test_anchor + test_block, 1)

CMAKE_PATH.write_text(cmake, encoding="utf-8")
print("league schedule extraction applied")
