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
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
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
include_anchor = '#include "../league_roster.h"\n'
if '#include "../league_waiver.h"' not in handler:
    if include_anchor not in handler:
        raise RuntimeError("league roster include anchor missing")
    handler = handler.replace(
        include_anchor,
        include_anchor + '#include "../league_waiver.h"\n',
        1,
    )

for signature in (
    "bool waiverModeActive(const Json::Value &rules)",
    "bool waiverDeadlinePassed(const Json::Value &rules)",
):
    if signature in handler:
        handler = remove_function(handler, signature)

handler = re.sub(
    r"(?<![A-Za-z0-9_:])waiverModeActive\(",
    "cff::league_waiver::modeActive(",
    handler,
)
handler = re.sub(
    r"(?<![A-Za-z0-9_:])waiverDeadlinePassed\(",
    "cff::league_waiver::deadlinePassed(",
    handler,
)
if handler.count("isoNow(") == 1:
    handler = remove_function(handler, "std::string isoNow()")

claim_order_block = '''    int claimOrder = 1;
    for (const auto &existing : claims) {
        if (jsonString(existing, "managerEmail") == accountEmail && jsonString(existing, "status") == "Pending") {
            claimOrder = std::max(claimOrder, cff::getIntOrDefault(existing, "claimOrder", 1) + 1);
        }
    }
'''
claim_order_replacement = (
    "    const auto claimOrder = cff::league_waiver::nextClaimOrder(claims, accountEmail);\n"
)
if claim_order_block in handler:
    handler = handler.replace(claim_order_block, claim_order_replacement, 1)
elif claim_order_replacement not in handler:
    raise RuntimeError("local waiver claim-order block missing")

sort_block = '''    std::vector<Json::ArrayIndex> claimIndexes;
    for (Json::ArrayIndex i = 0; i < claims.size(); ++i) {
        claimIndexes.push_back(i);
    }
    std::sort(claimIndexes.begin(), claimIndexes.end(), [&claims](Json::ArrayIndex left, Json::ArrayIndex right) {
        const auto leftPriority = cff::getIntOrDefault(claims[left], "priority", 999);
        const auto rightPriority = cff::getIntOrDefault(claims[right], "priority", 999);
        if (leftPriority != rightPriority) return leftPriority < rightPriority;
        return cff::getIntOrDefault(claims[left], "claimOrder", 999) < cff::getIntOrDefault(claims[right], "claimOrder", 999);
    });
'''
sort_replacement = (
    "    const auto claimIndexes = cff::league_waiver::orderedClaimIndexes(claims);\n"
)
if sort_block in handler:
    handler = handler.replace(sort_block, sort_replacement, 1)
elif sort_replacement not in handler:
    raise RuntimeError("local waiver processing-order block missing")

priority_block = '''    Json::Value priorities(Json::arrayValue);
    int priority = 1;
    for (const auto &member : arrayForLeague(membersByLeague, leagueId)) {
        if (lowerString(jsonString(member, "status", "Active")) == "removed") continue;
        Json::Value item;
        item["managerEmail"] = jsonString(member, "email");
        item["role"] = jsonString(member, "role", "member");
        item["status"] = jsonString(member, "status", "Active");
        item["priority"] = priority++;
        priorities.append(item);
    }
'''
priority_replacement = (
    "    auto priorities = cff::league_waiver::buildPriorityBoard(\n"
    "        arrayForLeague(membersByLeague, leagueId));\n"
)
priority_count = handler.count(priority_block)
if priority_count:
    if priority_count != 2:
        raise RuntimeError(f"expected two local priority-board blocks, found {priority_count}")
    handler = handler.replace(priority_block, priority_replacement)
elif handler.count("cff::league_waiver::buildPriorityBoard(") < 2:
    raise RuntimeError("local waiver priority-board blocks missing")

summary_replacements = {
    '"Claimed " + jsonString(player, "name")':
        "cff::league_waiver::claimTransactionSummary(player)",
    '"Claimed " + jsonString(claim["addPlayer"], "name")':
        "cff::league_waiver::claimTransactionSummary(claim[\"addPlayer\"])",
    '"Added " + jsonString(player, "name")':
        "cff::league_waiver::processedTransactionSummary(player)",
    '"Cancelled waiver claim"':
        "cff::league_waiver::cancelledTransactionSummary()",
    '"Reset waiver priority order"':
        "cff::league_waiver::resetPriorityTransactionSummary()",
}
for old, new in summary_replacements.items():
    if old in handler:
        handler = handler.replace(old, new)

# The generic processed-summary replacement also matches the local free-agent
# transaction. Keep that non-waiver path owned by the handler.
handler = handler.replace(
    'addTransactionLocked(leagueId, "Free Agent", cff::league_waiver::processedTransactionSummary(player), accountEmail);',
    'addTransactionLocked(leagueId, "Free Agent", "Added " + jsonString(player, "name"), accountEmail);',
)

for forbidden in (
    "bool waiverModeActive(const Json::Value &rules)",
    "bool waiverDeadlinePassed(const Json::Value &rules)",
    "std::sort(claimIndexes.begin()",
):
    if forbidden in handler:
        raise RuntimeError(f"waiver policy implementation remains in handler: {forbidden}")

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
        raise RuntimeError(
            f"expected at least {minimum} delegations to {symbol}, found {actual}"
        )

HANDLER_PATH.write_text(handler, encoding="utf-8")

cmake = CMAKE_PATH.read_text(encoding="utf-8")
if "    src/league_waiver.cpp\n" not in cmake:
    source_anchor = "    src/league_roster.cpp\n"
    if source_anchor not in cmake:
        raise RuntimeError("league roster source anchor missing")
    cmake = cmake.replace(source_anchor, source_anchor + "    src/league_waiver.cpp\n", 1)

if "add_executable(league_waiver_tests" not in cmake:
    test_anchor = "    add_test(NAME league_roster_tests COMMAND league_roster_tests)\n"
    if test_anchor not in cmake:
        raise RuntimeError("league roster test anchor missing")
    test_block = '''

    add_executable(league_waiver_tests
        tests/league_waiver_tests.cpp
        src/league_waiver.cpp
        src/json_utils.cpp
    )
    target_include_directories(league_waiver_tests PRIVATE src)
    target_link_libraries(league_waiver_tests PRIVATE Drogon::Drogon)
    add_test(NAME league_waiver_tests COMMAND league_waiver_tests)
'''
    cmake = cmake.replace(test_anchor, test_anchor + test_block, 1)

CMAKE_PATH.write_text(cmake, encoding="utf-8")
print("league waiver policy extraction applied")
