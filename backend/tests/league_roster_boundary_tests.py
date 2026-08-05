#!/usr/bin/env python3
"""Structural contracts for league roster and lineup policy ownership."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HANDLER = (ROOT / "backend/src/handlers/league_handler.cpp").read_text(encoding="utf-8")
HEADER = (ROOT / "backend/src/league_roster.h").read_text(encoding="utf-8")
SOURCE = (ROOT / "backend/src/league_roster.cpp").read_text(encoding="utf-8")
CMAKE = (ROOT / "backend/CMakeLists.txt").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


require('#include "../league_roster.h"' in HANDLER, "league handler must include league_roster.h")

owned_functions = (
    "flexEligible",
    "slotLimit",
    "playerEligibleForSlot",
    "validateRosterSlotMove",
    "lineupErrorsFromCounts",
    "rosterLimitFromRules",
    "preferredRosterSlot",
)
for function in owned_functions:
    require(f"{function}(" in HEADER, f"league roster interface missing {function}")
    require(f"{function}(" in SOURCE, f"league roster implementation missing {function}")

for leaked_implementation in (
    "bool flexEligible(const std::string &position)",
    "int slotLimit(const Json::Value &rules, const std::string &slot)",
    "bool playerEligibleForSlot(const Json::Value &player, const std::string &slot)",
    "bool validateRosterSlotMove(const Json::Value &player,",
    "Json::Value lineupErrorsFromCounts(",
    "int rosterLimitFromRules(const Json::Value &rules)",
):
    require(leaked_implementation not in HANDLER,
            f"league roster policy leaked into HTTP handler: {leaked_implementation}")

for delegation in (
    "cff::league_roster::validateRosterSlotMove(",
    "cff::league_roster::lineupErrorsFromCounts(",
    "cff::league_roster::rosterLimitFromRules(",
    "cff::league_roster::preferredRosterSlot(",
):
    require(delegation in HANDLER, f"league handler delegation missing: {delegation}")

require(
    '"SELECT roster_slot, COUNT(*) FROM rosters WHERE league_id = $1 AND manager_email = $2 GROUP BY roster_slot"'
    in HANDLER,
    "roster count query changed during policy extraction",
)
require(
    "return cff::league_roster::preferredRosterSlot(player, rules, counts, offset);" in HANDLER,
    "database-backed slot assignment must delegate only its pure selection step",
)

for forbidden_dependency in (
    "drogon::",
    "PGconn",
    "PQexec",
    "DB_URL",
    "SELECT ",
    "INSERT ",
    "UPDATE ",
    "DELETE ",
):
    require(forbidden_dependency not in SOURCE,
            f"pure league roster module gained infrastructure dependency: {forbidden_dependency}")

for behavior_contract in (
    'normalized == "rb" || normalized == "wr" || normalized == "te"',
    'slot == "bench"',
    'slot == "flex"',
    'slot != "qb"',
    '"Missing " + std::to_string(required - filled)',
    '"Too many " + upperString(slot) + " starter(s)"',
    "return 14;",
    'cff::getIntOrDefault(rules, "bench", 6)',
    'return "flex";',
    'return "bench";',
    "return std::nullopt;",
):
    require(behavior_contract in SOURCE,
            f"league roster behavior contract missing: {behavior_contract}")

for cmake_contract in (
    "src/league_roster.cpp",
    "tests/league_roster_tests.cpp",
    "add_executable(league_roster_tests",
    "add_test(NAME league_roster_tests COMMAND league_roster_tests)",
):
    require(cmake_contract in CMAKE, f"CMake league roster contract missing: {cmake_contract}")

print("league roster policy boundary contracts passed")
