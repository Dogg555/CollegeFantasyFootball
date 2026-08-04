#include "waiver_lifecycle.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void expect(bool condition, const std::string &message) {
    if (!condition) {
        std::cerr << "waiver_lifecycle_tests failed: " << message << std::endl;
        std::exit(1);
    }
}

Json::Value claim(const std::string &id,
                  const std::string &manager,
                  int order,
                  const std::string &createdAt) {
    Json::Value value(Json::objectValue);
    value["id"] = id;
    value["managerEmail"] = manager;
    value["status"] = "pending";
    value["claimOrder"] = order;
    value["createdAt"] = createdAt;
    return value;
}

void testVersionContracts() {
    Json::Value body(Json::objectValue);
    expect(!cff::waiver_lifecycle::expectedVersionMatches(4, body, true),
           "missing required version must fail closed");
    expect(cff::waiver_lifecycle::expectedVersionMatches(4, body, false),
           "legacy optional version must remain compatible");
    body["expectedVersion"] = 4;
    expect(cff::waiver_lifecycle::expectedVersionMatches(4, body, true),
           "matching version was rejected");
    body["expectedVersion"] = 3;
    expect(!cff::waiver_lifecycle::expectedVersionMatches(4, body, true),
           "stale version was accepted");
}

void testExactReorderContracts() {
    Json::Value claims(Json::arrayValue);
    claims.append(claim("a", "manager@example.com", 1, "2026-08-04T00:00:00Z"));
    claims.append(claim("b", "manager@example.com", 2, "2026-08-04T00:01:00Z"));
    claims.append(claim("c", "other@example.com", 1, "2026-08-04T00:02:00Z"));

    Json::Value valid(Json::arrayValue);
    valid.append("b");
    valid.append("a");
    expect(cff::waiver_lifecycle::validClaimReorder(claims, valid, "MANAGER@example.com"),
           "exact manager claim reorder was rejected");

    Json::Value missing(Json::arrayValue);
    missing.append("a");
    expect(!cff::waiver_lifecycle::validClaimReorder(claims, missing, "manager@example.com"),
           "partial reorder must fail");

    Json::Value duplicate(Json::arrayValue);
    duplicate.append("a");
    duplicate.append("a");
    expect(!cff::waiver_lifecycle::validClaimReorder(claims, duplicate, "manager@example.com"),
           "duplicate claim IDs must fail");
}

void testPriorityRotationContracts() {
    Json::Value members(Json::arrayValue);
    Json::Value commissioner(Json::objectValue);
    commissioner["email"] = "owner@example.com";
    commissioner["role"] = "commissioner";
    commissioner["status"] = "active";
    commissioner["joinedAt"] = "2026-08-01T00:00:00Z";
    members.append(commissioner);
    Json::Value member(Json::objectValue);
    member["email"] = "member@example.com";
    member["role"] = "member";
    member["status"] = "active";
    member["joinedAt"] = "2026-08-02T00:00:00Z";
    members.append(member);

    auto priorities = cff::waiver_lifecycle::canonicalPriorityBoard(members);
    expect(priorities.size() == 2, "priority board omitted active managers");
    expect(priorities[0]["managerEmail"].asString() == "owner@example.com",
           "commissioner seed order changed");

    Json::Value claims(Json::arrayValue);
    claims.append(claim("owner-1", "owner@example.com", 1, "2026-08-04T00:00:00Z"));
    claims.append(claim("owner-2", "owner@example.com", 2, "2026-08-04T00:01:00Z"));
    claims.append(claim("member-1", "member@example.com", 1, "2026-08-04T00:02:00Z"));

    auto order = cff::waiver_lifecycle::orderedClaimIndexes(claims, priorities);
    expect(claims[order[0]]["id"].asString() == "owner-1",
           "top-priority manager first claim changed");

    cff::waiver_lifecycle::moveManagerToBack(priorities, "owner@example.com");
    order = cff::waiver_lifecycle::orderedClaimIndexes(claims, priorities);
    expect(claims[order[0]]["id"].asString() == "member-1",
           "winning manager remaining claims did not move behind other managers");
    expect(priorities[0]["priority"].asInt() == 1
           && priorities[1]["priority"].asInt() == 2,
           "priority rotation did not remain dense");
}

void testIdentityNormalization() {
    expect(cff::waiver_lifecycle::canonicalEmail("  USER@Example.COM ") == "user@example.com",
           "email normalization changed");
    expect(cff::waiver_lifecycle::canonicalPlayerId(" Player-123 ") == "player-123",
           "player ID normalization changed");
}

} // namespace

int main() {
    testVersionContracts();
    testExactReorderContracts();
    testPriorityRotationContracts();
    testIdentityNormalization();
    std::cout << "waiver lifecycle policy contracts passed" << std::endl;
    return 0;
}
