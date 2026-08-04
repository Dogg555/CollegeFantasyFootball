#!/usr/bin/env python3
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / "backend/src/league_waiver.h"
SOURCE = ROOT / "backend/src/league_waiver.cpp"
HANDLER = ROOT / "backend/src/handlers/league_handler.cpp"
CMAKE = ROOT / "backend/CMakeLists.txt"


def fail(message: str) -> None:
    print(f"league_waiver_boundary_tests failed: {message}", file=sys.stderr)
    raise SystemExit(1)


for path in (HEADER, SOURCE, HANDLER, CMAKE):
    if not path.exists():
        fail(f"missing required file: {path.relative_to(ROOT)}")

header = HEADER.read_text(encoding="utf-8")
source = SOURCE.read_text(encoding="utf-8")
handler = HANDLER.read_text(encoding="utf-8")
cmake = CMAKE.read_text(encoding="utf-8")

for symbol in (
    "modeActive",
    "deadlinePassedAt",
    "deadlinePassed",
    "nextClaimOrder",
    "orderedClaimIndexes",
    "buildPriorityBoard",
    "claimTransactionSummary",
    "processedTransactionSummary",
    "cancelledTransactionSummary",
    "resetPriorityTransactionSummary",
):
    if symbol not in header:
        fail(f"league waiver header does not declare {symbol}")
    if f"{symbol}(" not in source:
        fail(f"league waiver source does not define {symbol}")

for forbidden in (
    "drogon::",
    "PGconn",
    "PQexec",
    "SELECT ",
    "INSERT ",
    "UPDATE ",
    "DELETE ",
):
    if forbidden in header or forbidden in source:
        fail(f"league waiver domain module leaked infrastructure dependency: {forbidden}")

if '#include "../league_waiver.h"' not in handler:
    fail("league handler does not include the waiver policy module")

for forbidden in (
    "bool waiverModeActive(const Json::Value &rules)",
    "bool waiverDeadlinePassed(const Json::Value &rules)",
    "std::sort(claimIndexes.begin()",
):
    if forbidden in handler:
        fail(f"waiver policy implementation remains in league handler: {forbidden}")

required_delegations = {
    "modeActive": 3,
    "deadlinePassed": 4,
    "nextClaimOrder": 1,
    "orderedClaimIndexes": 1,
    "buildPriorityBoard": 2,
    "claimTransactionSummary": 2,
    "processedTransactionSummary": 3,
    "cancelledTransactionSummary": 2,
    "resetPriorityTransactionSummary": 2,
}
for symbol, minimum in required_delegations.items():
    actual = handler.count(f"cff::league_waiver::{symbol}(")
    if actual < minimum:
        fail(f"expected at least {minimum} handler delegations to {symbol}, found {actual}")

if "    src/league_waiver.cpp\n" not in cmake:
    fail("production CMake target does not include league_waiver.cpp")
if "add_executable(league_waiver_tests" not in cmake:
    fail("CMake does not register league waiver tests")
if "tests/league_waiver_tests.cpp" not in cmake:
    fail("CMake league waiver target does not compile the permanent tests")

print("league waiver policy boundary contracts passed")
