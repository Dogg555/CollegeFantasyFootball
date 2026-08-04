#include "league_waiver.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void expect(bool condition, const std::string &message) {
    if (!condition) {
        std::cerr << "league_waiver_tests failed: " << message << std::endl;
        std::exit(1);
    }
}

Json::Value rules(const std::string &mode = "free_agency",
                  bool locked = false,
                  const std::string &deadline = "") {
    Json::Value value(Json::objectValue);
    value["mode"] = mode;
    value["freeAgencyLocked"] = locked;
    value["claimDeadline"] = deadline;
    return value;
}

Json::Value claim(const std::string &id,
                  const std::string &manager,
                  const std::string &status,
                  int priority,
                  int order) {
    Json::Value value(Json::objectValue);
    value["id"] = id;
    value["managerEmail"] = manager;
    value["status"] = status;
    value["priority"] = priority;
    value["claimOrder"] = order;
    return value;
}

Json::Value member(const std::string &email,
                   const std::string &status = "Active",
                   const std::string &role = "member") {
    Json::Value value(Json::objectValue);
    value["email"] = email;
    value["status"] = status;
    value["role"] = role;
    return value;
}

void testWaiverModePolicy() {
    expect(!cff::league_waiver::modeActive(Json::Value{Json::objectValue}),
           "empty rules must preserve free-agency mode");
    expect(!cff::league_waiver::modeActive(rules()),
           "free agency must remain available when unlocked");
    expect(cff::league_waiver::modeActive(rules("waivers")),
           "waiver mode must lock direct free-agent additions");
    expect(cff::league_waiver::modeActive(rules("free_agency", true)),
           "explicit free-agency lock must activate waivers");
    expect(!cff::league_waiver::modeActive(rules("Waivers")),
           "mode matching must remain case-sensitive for compatibility");
}

void testDeadlinePolicy() {
    const auto now = std::string{"2026-08-03T20:17"};
    expect(cff::league_waiver::deadlinePassedAt(rules(), now),
           "empty claim deadline must be immediately processable");
    expect(cff::league_waiver::deadlinePassedAt(
               rules("waivers", false, "2026-08-03T20:16"), now),
           "past deadline must be processable");
    expect(cff::league_waiver::deadlinePassedAt(
               rules("waivers", false, "2026-08-03T20:17"), now),
           "deadline equal to the current minute must be processable");
    expect(!cff::league_waiver::deadlinePassedAt(
               rules("waivers", false, "2026-08-03T20:18"), now),
           "future deadline must remain blocked");
}

void testClaimOrderPolicy() {
    Json::Value claims(Json::arrayValue);
    claims.append(claim("a", "manager@example.com", "Pending", 1, 1));
    claims.append(claim("b", "other@example.com", "Pending", 1, 8));
    claims.append(claim("c", "manager@example.com", "Cancelled", 1, 9));
    claims.append(claim("d", "manager@example.com", "Pending", 1, 4));

    expect(cff::league_waiver::nextClaimOrder(claims, "manager@example.com") == 5,
           "next claim order must follow the manager's highest pending order");
    expect(cff::league_waiver::nextClaimOrder(claims, "new@example.com") == 1,
           "manager without pending claims must start at one");
}

void testProcessingOrderPolicy() {
    Json::Value claims(Json::arrayValue);
    claims.append(claim("priority-two", "a@example.com", "Pending", 2, 1));
    claims.append(claim("priority-one-third", "b@example.com", "Pending", 1, 3));
    claims.append(claim("priority-one-first", "c@example.com", "Pending", 1, 1));
    Json::Value fallback(Json::objectValue);
    fallback["id"] = "fallback";
    claims.append(fallback);

    const auto indexes = cff::league_waiver::orderedClaimIndexes(claims);
    expect(indexes.size() == 4, "all claims must remain represented in the processing order");
    const std::vector<std::string> expected{
        "priority-one-first",
        "priority-one-third",
        "priority-two",
        "fallback"
    };
    for (std::size_t index = 0; index < expected.size(); ++index) {
        expect(claims[indexes[index]]["id"].asString() == expected[index],
               "priority then claim-order sorting changed at index " + std::to_string(index));
    }
}

void testPriorityBoardPolicy() {
    Json::Value members(Json::arrayValue);
    members.append(member("owner@example.com", "Active", "commissioner"));
    members.append(member("invited@example.com", "Invited"));
    members.append(member("removed-upper@example.com", "Removed"));
    members.append(member("removed-lower@example.com", "removed"));
    Json::Value defaults(Json::objectValue);
    defaults["email"] = "defaults@example.com";
    members.append(defaults);

    const auto board = cff::league_waiver::buildPriorityBoard(members);
    expect(board.size() == 3, "removed members must be excluded case-insensitively");
    expect(board[0]["managerEmail"].asString() == "owner@example.com"
               && board[0]["priority"].asInt() == 1
               && board[0]["role"].asString() == "commissioner",
           "commissioner priority entry changed");
    expect(board[1]["managerEmail"].asString() == "invited@example.com"
               && board[1]["priority"].asInt() == 2
               && board[1]["status"].asString() == "Invited",
           "legacy non-removed membership compatibility changed");
    expect(board[2]["managerEmail"].asString() == "defaults@example.com"
               && board[2]["priority"].asInt() == 3
               && board[2]["role"].asString() == "member"
               && board[2]["status"].asString() == "Active",
           "priority board defaults changed");
}

void testTransactionSummaries() {
    Json::Value player(Json::objectValue);
    player["name"] = "Test Runner";
    expect(cff::league_waiver::claimTransactionSummary(player) == "Claimed Test Runner",
           "waiver claim summary changed");
    expect(cff::league_waiver::processedTransactionSummary(player) == "Added Test Runner",
           "waiver processed summary changed");
    expect(cff::league_waiver::cancelledTransactionSummary() == "Cancelled waiver claim",
           "waiver cancellation summary changed");
    expect(cff::league_waiver::resetPriorityTransactionSummary() == "Reset waiver priority order",
           "waiver priority reset summary changed");
}

} // namespace

int main() {
    testWaiverModePolicy();
    testDeadlinePolicy();
    testClaimOrderPolicy();
    testProcessingOrderPolicy();
    testPriorityBoardPolicy();
    testTransactionSummaries();
    std::cout << "league waiver policy contracts passed" << std::endl;
    return 0;
}
