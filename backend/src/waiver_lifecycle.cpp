#include "waiver_lifecycle.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace cff::waiver_lifecycle {

namespace {

std::string trim(std::string value) {
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), value.end());
    return value;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

int intValue(const Json::Value &value, const char *key, int fallback) {
    const auto &node = value[key];
    return node.isInt() || node.isUInt() ? node.asInt() : fallback;
}

std::string stringValue(const Json::Value &value,
                        const char *key,
                        const std::string &fallback = "") {
    const auto &node = value[key];
    return node.isString() ? node.asString() : fallback;
}

} // namespace

std::string canonicalEmail(std::string value) {
    return lower(trim(std::move(value)));
}

std::string canonicalPlayerId(std::string value) {
    return lower(trim(std::move(value)));
}

long long normalizedVersion(const Json::Value &value, long long fallback) {
    if (value.isInt64() || value.isUInt64()) return value.asInt64();
    if (value.isInt() || value.isUInt()) return value.asInt64();
    if (value.isString()) {
        try {
            return std::stoll(value.asString());
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

bool expectedVersionMatches(long long currentVersion,
                            const Json::Value &body,
                            bool required) {
    if (!body.isObject() || !body.isMember("expectedVersion")) {
        return !required;
    }
    return normalizedVersion(body["expectedVersion"], -1) == currentVersion;
}

bool validClaimReorder(const Json::Value &pendingClaims,
                       const Json::Value &claimIds,
                       const std::string &managerEmail) {
    if (!pendingClaims.isArray() || !claimIds.isArray()) return false;
    const auto email = canonicalEmail(managerEmail);
    std::unordered_set<std::string> expected;
    for (const auto &claim : pendingClaims) {
        if (canonicalEmail(stringValue(claim, "managerEmail")) != email) continue;
        if (lower(stringValue(claim, "status", "pending")) != "pending") continue;
        const auto id = trim(stringValue(claim, "id"));
        if (!id.empty()) expected.insert(id);
    }
    if (expected.size() != claimIds.size()) return false;
    std::unordered_set<std::string> seen;
    for (const auto &idValue : claimIds) {
        if (!idValue.isString()) return false;
        const auto id = trim(idValue.asString());
        if (id.empty() || !expected.count(id) || !seen.insert(id).second) return false;
    }
    return seen.size() == expected.size();
}

Json::Value canonicalPriorityBoard(const Json::Value &members) {
    struct Member {
        std::string email;
        std::string role;
        std::string joinedAt;
    };
    std::vector<Member> active;
    if (members.isArray()) {
        for (const auto &member : members) {
            const auto status = lower(stringValue(member, "status", "active"));
            if (status != "active") continue;
            const auto email = canonicalEmail(stringValue(member, "email"));
            if (email.empty()) continue;
            active.push_back({email, lower(stringValue(member, "role", "member")), stringValue(member, "joinedAt")});
        }
    }
    std::sort(active.begin(), active.end(), [](const Member &left, const Member &right) {
        const auto leftCommissioner = left.role == "commissioner" ? 0 : 1;
        const auto rightCommissioner = right.role == "commissioner" ? 0 : 1;
        return std::make_tuple(leftCommissioner, left.joinedAt, left.email)
            < std::make_tuple(rightCommissioner, right.joinedAt, right.email);
    });
    Json::Value board(Json::arrayValue);
    int priority = 1;
    for (const auto &member : active) {
        Json::Value item(Json::objectValue);
        item["managerEmail"] = member.email;
        item["priority"] = priority++;
        board.append(item);
    }
    return board;
}

void moveManagerToBack(Json::Value &priorityBoard,
                       const std::string &managerEmail) {
    if (!priorityBoard.isArray()) return;
    const auto target = canonicalEmail(managerEmail);
    std::vector<Json::Value> ordered;
    Json::Value moved;
    for (const auto &entry : priorityBoard) {
        if (canonicalEmail(stringValue(entry, "managerEmail")) == target) moved = entry;
        else ordered.push_back(entry);
    }
    if (moved.isObject()) ordered.push_back(moved);
    priorityBoard = Json::Value{Json::arrayValue};
    int priority = 1;
    for (auto &entry : ordered) {
        entry["managerEmail"] = canonicalEmail(stringValue(entry, "managerEmail"));
        entry["priority"] = priority++;
        priorityBoard.append(entry);
    }
}

std::vector<Json::ArrayIndex> orderedClaimIndexes(const Json::Value &claims,
                                                  const Json::Value &priorityBoard) {
    std::unordered_map<std::string, int> priorities;
    if (priorityBoard.isArray()) {
        for (const auto &entry : priorityBoard) {
            priorities[canonicalEmail(stringValue(entry, "managerEmail"))]
                = intValue(entry, "priority", std::numeric_limits<int>::max());
        }
    }
    std::vector<Json::ArrayIndex> indexes;
    if (!claims.isArray()) return indexes;
    indexes.reserve(claims.size());
    for (Json::ArrayIndex index = 0; index < claims.size(); ++index) indexes.push_back(index);
    std::sort(indexes.begin(), indexes.end(), [&](Json::ArrayIndex left, Json::ArrayIndex right) {
        const auto leftEmail = canonicalEmail(stringValue(claims[left], "managerEmail"));
        const auto rightEmail = canonicalEmail(stringValue(claims[right], "managerEmail"));
        const auto leftPriority = priorities.count(leftEmail)
            ? priorities[leftEmail]
            : intValue(claims[left], "priority", std::numeric_limits<int>::max());
        const auto rightPriority = priorities.count(rightEmail)
            ? priorities[rightEmail]
            : intValue(claims[right], "priority", std::numeric_limits<int>::max());
        return std::make_tuple(leftPriority,
                               intValue(claims[left], "claimOrder", std::numeric_limits<int>::max()),
                               stringValue(claims[left], "createdAt"),
                               stringValue(claims[left], "id"))
            < std::make_tuple(rightPriority,
                              intValue(claims[right], "claimOrder", std::numeric_limits<int>::max()),
                              stringValue(claims[right], "createdAt"),
                              stringValue(claims[right], "id"));
    });
    return indexes;
}

} // namespace cff::waiver_lifecycle
