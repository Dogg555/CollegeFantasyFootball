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
include_anchor = '#include "../league_schedule.h"\n'
if '#include "../league_roster.h"' not in handler:
    if include_anchor not in handler:
        raise RuntimeError("league schedule include anchor missing")
    handler = handler.replace(
        include_anchor,
        include_anchor + '#include "../league_roster.h"\n',
        1,
    )

for signature in (
    "bool flexEligible(const std::string &position)",
    "int slotLimit(const Json::Value &rules, const std::string &slot)",
    "bool playerEligibleForSlot(const Json::Value &player, const std::string &slot)",
    "bool validateRosterSlotMove(const Json::Value &player,",
    "Json::Value lineupErrorsFromCounts(const std::string &managerEmail,",
    "int rosterLimitFromRules(const Json::Value &rules)",
):
    if signature in handler:
        handler = remove_function(handler, signature)

if handler.count("upperString(") == 1:
    handler = remove_function(handler, "std::string upperString(std::string value)")

assignment_start_text = '    const auto position = lowerString(jsonString(player, "position", "flex"));\n'
assignment_start = handler.find(assignment_start_text)
if assignment_start < 0:
    if "return cff::league_roster::preferredRosterSlot(player, rules, counts, offset);" not in handler:
        raise RuntimeError("database roster-slot selection block missing")
else:
    assignment_end_text = "    return std::nullopt;\n"
    assignment_end = handler.find(assignment_end_text, assignment_start)
    if assignment_end < 0:
        raise RuntimeError("database roster-slot selection return missing")
    assignment_end += len(assignment_end_text)
    handler = (
        handler[:assignment_start]
        + "    return cff::league_roster::preferredRosterSlot(player, rules, counts, offset);\n"
        + handler[assignment_end:]
    )

for name in (
    "validateRosterSlotMove",
    "lineupErrorsFromCounts",
    "rosterLimitFromRules",
):
    handler = re.sub(
        rf"(?<![A-Za-z0-9_:]){name}\(",
        f"cff::league_roster::{name}(",
        handler,
    )

for forbidden in (
    "bool flexEligible(const std::string &position)",
    "int slotLimit(const Json::Value &rules, const std::string &slot)",
    "bool playerEligibleForSlot(const Json::Value &player, const std::string &slot)",
    "bool validateRosterSlotMove(const Json::Value &player,",
    "Json::Value lineupErrorsFromCounts(const std::string &managerEmail,",
    "int rosterLimitFromRules(const Json::Value &rules)",
):
    if forbidden in handler:
        raise RuntimeError(f"league roster implementation remains in handler: {forbidden}")

for required in (
    "cff::league_roster::validateRosterSlotMove(",
    "cff::league_roster::lineupErrorsFromCounts(",
    "cff::league_roster::rosterLimitFromRules(",
    "cff::league_roster::preferredRosterSlot(",
):
    if required not in handler:
        raise RuntimeError(f"league roster delegation missing: {required}")

HANDLER_PATH.write_text(handler, encoding="utf-8")

cmake = CMAKE_PATH.read_text(encoding="utf-8")
if "    src/league_roster.cpp\n" not in cmake:
    source_anchor = "    src/league_schedule.cpp\n"
    if source_anchor not in cmake:
        raise RuntimeError("league schedule source anchor missing")
    cmake = cmake.replace(
        source_anchor,
        source_anchor + "    src/league_roster.cpp\n",
        1,
    )

if "add_executable(league_roster_tests" not in cmake:
    test_anchor = "    add_test(NAME league_schedule_tests COMMAND league_schedule_tests)\n"
    if test_anchor not in cmake:
        raise RuntimeError("league schedule test anchor missing")
    test_block = """

    add_executable(league_roster_tests
        tests/league_roster_tests.cpp
        src/league_roster.cpp
        src/json_utils.cpp
    )
    target_include_directories(league_roster_tests PRIVATE src)
    target_link_libraries(league_roster_tests PRIVATE Drogon::Drogon)
    add_test(NAME league_roster_tests COMMAND league_roster_tests)
"""
    cmake = cmake.replace(test_anchor, test_anchor + test_block, 1)

CMAKE_PATH.write_text(cmake, encoding="utf-8")
print("league roster policy extraction applied")
