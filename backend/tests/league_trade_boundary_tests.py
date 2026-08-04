#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HANDLER = ROOT / "backend/src/handlers/league_handler.cpp"
HEADER = ROOT / "backend/src/league_trade.h"
SOURCE = ROOT / "backend/src/league_trade.cpp"
CMAKE = ROOT / "backend/CMakeLists.txt"
WORKFLOW = ROOT / ".github/workflows/league-trade-contracts.yml"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


for path in (HANDLER, HEADER, SOURCE, CMAKE, WORKFLOW):
    require(path.exists(), f"missing required file: {path.relative_to(ROOT)}")

handler = HANDLER.read_text(encoding="utf-8")
header = HEADER.read_text(encoding="utf-8")
source = SOURCE.read_text(encoding="utf-8")
cmake = CMAKE.read_text(encoding="utf-8")
workflow = WORKFLOW.read_text(encoding="utf-8")

require('#include "../league_trade.h"' in handler,
        "league handler must include the trade policy module")
require("namespace cff::league_trade" in header,
        "trade policy header must use the league_trade namespace")
require("namespace cff::league_trade" in source,
        "trade policy implementation must use the league_trade namespace")

symbols = (
    "approvalRequired",
    "expirationHours",
    "validTarget",
    "requestStatusAllowed",
    "potentiallyExecutes",
    "openStatus",
    "decideStatus",
    "playerLockedInOpenOffer",
    "offerTransactionSummary",
    "statusTransactionSummary",
)
for symbol in symbols:
    require(f"{symbol}(" in header, f"missing trade policy declaration: {symbol}")
    require(f"{symbol}(" in source, f"missing trade policy implementation: {symbol}")

for forbidden in (
    "bool tradeApprovalRequired(const Json::Value &rules)",
    "int tradeExpirationHours(const Json::Value &rules)",
    "bool playerLockedInTradeLocked(const std::string &leagueId",
):
    require(forbidden not in handler,
            f"trade policy implementation leaked into league handler: {forbidden}")

required_delegations = {
    "approvalRequired": 2,
    "expirationHours": 1,
    "validTarget": 3,
    "requestStatusAllowed": 1,
    "potentiallyExecutes": 1,
    "openStatus": 2,
    "decideStatus": 2,
    "playerLockedInOpenOffer": 2,
    "offerTransactionSummary": 2,
    "statusTransactionSummary": 2,
}
for symbol, minimum in required_delegations.items():
    actual = handler.count(f"cff::league_trade::{symbol}(")
    require(actual >= minimum,
            f"expected at least {minimum} handler delegations to {symbol}, found {actual}")

for forbidden in (
    "drogon/",
    "postgresql/",
    "libpq",
    "PGconn",
    "HttpRequest",
    "HttpResponse",
    "DB_URL",
    "std::mutex",
    "std::thread",
    "getenv(",
):
    require(forbidden not in header and forbidden not in source,
            f"trade policy module must remain infrastructure-free: {forbidden}")

require("    src/league_trade.cpp\n" in cmake,
        "production target must include league_trade.cpp")
require("add_executable(league_trade_tests" in cmake,
        "CMake must register deterministic trade policy tests")
require("tests/league_trade_tests.cpp" in cmake,
        "trade policy test target must include its test source")
require("src/league_trade.cpp" in cmake,
        "trade policy test target must include its implementation")

for expected in (
    "league_trade.h",
    "league_trade.cpp",
    "league_trade_tests.cpp",
    "league_trade_boundary_tests.py",
    "Compile deterministic trade policy tests",
    "Run trade policy tests",
):
    require(expected in workflow,
            f"permanent trade workflow is missing: {expected}")

require("contents: write" not in workflow,
        "permanent trade workflow must be read-only")
require("git push" not in workflow,
        "permanent trade workflow must never mutate branches")

print("league trade policy boundary contracts passed")
