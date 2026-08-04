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


def replace_once_or_require(source: str,
                            old: str,
                            new: str,
                            description: str) -> str:
    count = source.count(old)
    if count == 1:
        return source.replace(old, new, 1)
    if count == 0 and new in source:
        return source
    raise RuntimeError(f"expected one {description}, found {count}")


handler = HANDLER_PATH.read_text(encoding="utf-8")
include_anchor = '#include "../league_waiver.h"\n'
if '#include "../league_trade.h"' not in handler:
    if include_anchor not in handler:
        raise RuntimeError("league waiver include anchor missing")
    handler = handler.replace(
        include_anchor,
        include_anchor + '#include "../league_trade.h"\n',
        1,
    )

for signature in (
    "bool tradeApprovalRequired(const Json::Value &rules)",
    "int tradeExpirationHours(const Json::Value &rules)",
    "bool playerLockedInTradeLocked(const std::string &leagueId,",
):
    if signature in handler:
        handler = remove_function(handler, signature)

handler = re.sub(
    r"(?<![A-Za-z0-9_:])tradeApprovalRequired\(",
    "cff::league_trade::approvalRequired(",
    handler,
)
handler = re.sub(
    r"(?<![A-Za-z0-9_:])tradeExpirationHours\(",
    "cff::league_trade::expirationHours(",
    handler,
)

handler = replace_once_or_require(
    handler,
    "if (playerLockedInTradeLocked(leagueId, accountEmail, playerId)) {",
    "if (cff::league_trade::playerLockedInOpenOffer(\n"
    "            arrayForLeague(tradesByLeague, leagueId), accountEmail, playerId)) {",
    "drop-player trade lock check",
)
handler = replace_once_or_require(
    handler,
    'if (playerLockedInTradeLocked(leagueId, accountEmail, jsonString((*body)["offerPlayer"], "id"))) {',
    'if (cff::league_trade::playerLockedInOpenOffer(\n'
    '            arrayForLeague(tradesByLeague, leagueId),\n'
    '            accountEmail,\n'
    '            jsonString((*body)["offerPlayer"], "id"))) {',
    "trade-offer player lock check",
)

for old, new, description in (
    (
        "if (requestedTarget.empty() || requestedTarget == accountEmail) {",
        "if (!cff::league_trade::validTarget(accountEmail, requestedTarget)) {",
        "request target validation",
    ),
    (
        "if (target.empty() || target == accountEmail) return std::nullopt;",
        "if (!cff::league_trade::validTarget(accountEmail, target)) return std::nullopt;",
        "database target validation",
    ),
    (
        "if (targetManager.empty() || targetManager == accountEmail) {",
        "if (!cff::league_trade::validTarget(accountEmail, targetManager)) {",
        "local target validation",
    ),
):
    handler = replace_once_or_require(handler, old, new, description)

status_validation = '''    if (!(status == "Accepted" || status == "Approved" || status == "Vetoed" || status == "Declined" || status == "Cancelled")) {
'''
status_validation_replacement = '''    if (!cff::league_trade::requestStatusAllowed(status)) {
'''
handler = replace_once_or_require(
    handler,
    status_validation,
    status_validation_replacement,
    "trade request status validation",
)

potential_execution = '''        const auto dbStatus = statusForDb(status);
        if ((dbStatus == "accepted" || dbStatus == "approved")) {
'''
potential_execution_replacement = '''        if (cff::league_trade::potentiallyExecutes(status)) {
'''
handler = replace_once_or_require(
    handler,
    potential_execution,
    potential_execution_replacement,
    "trade execution precheck",
)

db_transition = '''    const auto currentStatus = cell(result.get(), 0, 7);
    if (currentStatus != "pending" && currentStatus != "accepted") return std::nullopt;
    const bool involved = accountEmail == offeredBy || accountEmail == offeredTo;
    const bool requiresApproval = cellBool(result.get(), 0, 6);
    auto nextStatus = statusForDb(status);
    bool executeTrade = false;
    if (nextStatus == "accepted") {
        if (!involved) return std::nullopt;
        executeTrade = !requiresApproval;
        if (executeTrade) nextStatus = "approved";
    } else if (nextStatus == "approved" || nextStatus == "vetoed") {
        if (!commissioner) return std::nullopt;
        executeTrade = nextStatus == "approved";
    } else if (nextStatus == "declined" || nextStatus == "cancelled") {
        if (!involved && !commissioner) return std::nullopt;
    } else {
        return std::nullopt;
    }
'''
db_transition_replacement = '''    const auto currentStatus = cell(result.get(), 0, 7);
    if (!cff::league_trade::openStatus(currentStatus)) return std::nullopt;
    const bool involved = accountEmail == offeredBy || accountEmail == offeredTo;
    const bool requiresApproval = cellBool(result.get(), 0, 6);
    const auto decision = cff::league_trade::decideStatus(
        status, requiresApproval, involved, commissioner, true);
    if (!decision.allowed) return std::nullopt;
    const auto &nextStatus = decision.databaseStatus;
    const bool executeTrade = decision.execute;
'''
handler = replace_once_or_require(
    handler,
    db_transition,
    db_transition_replacement,
    "database trade status transition",
)

local_transition = '''            const auto currentStatus = jsonString(offers[i], "status");
            if (currentStatus != "Pending" && currentStatus != "Accepted") {
                sendError(callback, drogon::k409Conflict, "Trade is no longer open");
                return;
            }
            const bool requiresApproval = offers[i].isMember("requiresApproval") && offers[i]["requiresApproval"].asBool();
            const bool commissioner = isCommissionerLocked(accountEmail, leagueId);
            bool executeTrade = false;
            if (status == "Accepted") {
                executeTrade = !requiresApproval;
                offers[i]["status"] = executeTrade ? "Approved" : "Accepted";
            } else if (status == "Approved" || status == "Vetoed") {
                if (!commissioner) {
                    sendError(callback, drogon::k403Forbidden, "Commissioner access required");
                    return;
                }
                executeTrade = status == "Approved";
                offers[i]["status"] = status;
            } else {
                offers[i]["status"] = status;
            }
'''
local_transition_replacement = '''            const auto currentStatus = jsonString(offers[i], "status");
            if (!cff::league_trade::openStatus(currentStatus)) {
                sendError(callback, drogon::k409Conflict, "Trade is no longer open");
                return;
            }
            const bool requiresApproval = offers[i].isMember("requiresApproval") && offers[i]["requiresApproval"].asBool();
            const bool commissioner = isCommissionerLocked(accountEmail, leagueId);
            const auto decision = cff::league_trade::decideStatus(
                status, requiresApproval, true, commissioner, false);
            if (!decision.allowed) {
                if (decision.commissionerRequired) {
                    sendError(callback, drogon::k403Forbidden, "Commissioner access required");
                } else {
                    sendError(callback, drogon::k404NotFound, "Trade offer not found");
                }
                return;
            }
            const bool executeTrade = decision.execute;
            offers[i]["status"] = decision.displayStatus;
'''
handler = replace_once_or_require(
    handler,
    local_transition,
    local_transition_replacement,
    "local trade status transition",
)

summary_replacements = (
    (
        'dbAddTransaction(conn.get(), leagueId, "Trade Offer", "Offered " + jsonString(*offer, "name"), accountEmail, *offer);',
        'dbAddTransaction(conn.get(), leagueId, "Trade Offer", cff::league_trade::offerTransactionSummary(*offer), accountEmail, *offer);',
        "database trade offer summary",
    ),
    (
        'addTransactionLocked(leagueId, "Trade Offer", "Offered " + jsonString(trade["offerPlayer"], "name"), accountEmail);',
        'addTransactionLocked(leagueId, "Trade Offer", cff::league_trade::offerTransactionSummary(trade["offerPlayer"]), accountEmail);',
        "local trade offer summary",
    ),
    (
        'dbAddTransaction(conn.get(), leagueId, "Trade", statusForUi(nextStatus) + ": " + jsonString(offer, "name"), accountEmail, offer);',
        'dbAddTransaction(conn.get(), leagueId, "Trade", cff::league_trade::statusTransactionSummary(decision.displayStatus, offer), accountEmail, offer);',
        "database trade status summary",
    ),
    (
        'addTransactionLocked(leagueId, "Trade", jsonString(offers[i], "status") + ": " + jsonString(offers[i]["offerPlayer"], "name"), accountEmail);',
        'addTransactionLocked(leagueId, "Trade", cff::league_trade::statusTransactionSummary(decision.displayStatus, offers[i]["offerPlayer"]), accountEmail);',
        "local trade status summary",
    ),
    (
        'trade["status"] = statusForUi(nextStatus);',
        'trade["status"] = decision.displayStatus;',
        "database response trade status",
    ),
)
for old, new, description in summary_replacements:
    handler = replace_once_or_require(handler, old, new, description)

for forbidden in (
    "bool tradeApprovalRequired(const Json::Value &rules)",
    "int tradeExpirationHours(const Json::Value &rules)",
    "bool playerLockedInTradeLocked(const std::string &leagueId,",
):
    if forbidden in handler:
        raise RuntimeError(f"trade policy implementation remains in handler: {forbidden}")

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
    if actual < minimum:
        raise RuntimeError(
            f"expected at least {minimum} delegations to {symbol}, found {actual}"
        )

HANDLER_PATH.write_text(handler, encoding="utf-8")

cmake = CMAKE_PATH.read_text(encoding="utf-8")
if "    src/league_trade.cpp\n" not in cmake:
    source_anchor = "    src/league_waiver.cpp\n"
    if source_anchor not in cmake:
        raise RuntimeError("league waiver source anchor missing")
    cmake = cmake.replace(
        source_anchor,
        source_anchor + "    src/league_trade.cpp\n",
        1,
    )

if "add_executable(league_trade_tests" not in cmake:
    test_anchor = "    add_test(NAME league_waiver_tests COMMAND league_waiver_tests)\n"
    if test_anchor not in cmake:
        raise RuntimeError("league waiver test anchor missing")
    test_block = '''

    add_executable(league_trade_tests
        tests/league_trade_tests.cpp
        src/league_trade.cpp
        src/json_utils.cpp
    )
    target_include_directories(league_trade_tests PRIVATE src)
    target_link_libraries(league_trade_tests PRIVATE Drogon::Drogon)
    add_test(NAME league_trade_tests COMMAND league_trade_tests)
'''
    cmake = cmake.replace(test_anchor, test_anchor + test_block, 1)

CMAKE_PATH.write_text(cmake, encoding="utf-8")
print("league trade policy extraction applied")
