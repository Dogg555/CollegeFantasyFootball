#!/usr/bin/env python3
"""Structural contracts for league scheduling and draft-turn policy ownership."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HANDLER = (ROOT / "backend/src/handlers/league_handler.cpp").read_text(encoding="utf-8")
HEADER = (ROOT / "backend/src/league_schedule.h").read_text(encoding="utf-8")
IMPLEMENTATION = (ROOT / "backend/src/league_schedule.cpp").read_text(encoding="utf-8")
TESTS = (ROOT / "backend/tests/league_schedule_tests.cpp").read_text(encoding="utf-8")
CMAKE = (ROOT / "backend/CMakeLists.txt").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


require('#include "../league_schedule.h"' in HANDLER, "league handler must include league_schedule.h")

owned_functions = (
    "Json::Value activeMembers(",
    "Json::Value buildMatchups(",
    "Json::Value buildSeasonSchedule(",
    "std::string currentDraftManager(",
)
for function in owned_functions:
    require(function in HEADER, f"league schedule interface missing {function}")
    require(function in IMPLEMENTATION, f"league schedule implementation missing {function}")
    require(function not in HANDLER, f"league scheduling implementation leaked into HTTP handler: {function}")

for delegation in (
    "cff::league_schedule::buildMatchups(",
    "cff::league_schedule::buildSeasonSchedule(",
    "cff::league_schedule::currentDraftManager(",
):
    require(delegation in HANDLER, f"league handler delegation missing: {delegation}")

required_behavior = (
    'status != "Removed" && status != "removed"',
    "emails.size() % 2 == 1",
    "teamCount > 1 ? teamCount - 1 : 1",
    "std::rotate(rotated.begin() + 1",
    "week % 2 == 0",
    'matchup["status"] = "scheduled"',
    "for (int week = 1; week <= weeks; ++week)",
    'lowerString(draftType) == "snake"',
    "round % 2 == 1",
)
for contract in required_behavior:
    require(contract in IMPLEMENTATION, f"league schedule behavior contract missing: {contract}")

for test_contract in (
    "testFourTeamRoundRobin",
    "testSixTeamRoundRobin",
    "testOddTeamByes",
    "testSnakeDraftTurns",
    "pairs.size() == 6",
    "pairs.size() == 15",
    "appearances[email] == 4",
):
    require(test_contract in TESTS, f"league schedule regression coverage missing: {test_contract}")

require("src/league_schedule.cpp" in CMAKE, "production target must compile league_schedule.cpp")
require("tests/league_schedule_tests.cpp" in CMAKE, "CTest must compile league_schedule_tests.cpp")
require("add_test(NAME league_schedule_tests" in CMAKE, "CTest must register league_schedule_tests")

print("league schedule boundary contracts passed")
