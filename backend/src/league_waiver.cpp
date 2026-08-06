#include "league_waiver.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <sstream>

#include "json_utils.h"

namespace cff::league_waiver {

namespace {

std::string lowerString(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string currentIsoMinute() {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm timeInfo{};
#ifdef _WIN32
    gmtime_s(&timeInfo, &now);
#else
    gmtime_r(&now, &timeInfo);
#endif
    std::ostringstream out;
    out << std::put_time(&timeInfo, "%Y-%m-%dT%H:%M");
    return out.str();
}

} // namespace

bool modeActive(const Json::Value &rules) {
    return cff::getStringOrDefault(rules, "mode", "free_agency") == "waivers"
        || (rules.isMember("freeAgencyLocked") && rules["freeAgencyLocked"].asBool());
}

bool deadlinePassedAt(const Json::Value &rules,
                      const std::string &currentIsoMinuteValue) {
    const auto deadline = cff::getStringOrDefault(rules, "claimDeadline");
    if (deadline.empty()) {
        return true;
    }
    return deadline <= currentIsoMinuteValue;
}

bool deadlinePassed(const Json::Value &rules) {
    return deadlinePassedAt(rules, currentIsoMinute());
}

int nextClaimOrder(const Json::Value &claims,
                   const std::string &managerEmail) {
    int order = 1;
    for (const auto &claim : claims) {
        if (cff::getStringOrDefault(claim, "managerEmail") == managerEmail
            && cff::getStringOrDefault(claim, "status") == "Pending") {
            order = std::max(order, cff::getIntOrDefault(claim, "claimOrder", 1) + 1);
        }
    }
    return order;
}

std::vector<Json::ArrayIndex> orderedClaimIndexes(const Json::Value &claims) {
    std::vector<Json::ArrayIndex> indexes;
    indexes.reserve(claims.size());
    for (Json::ArrayIndex index = 0; index < claims.size(); ++index) {
        indexes.push_back(index);
    }
    std::sort(indexes.begin(), indexes.end(), [&claims](Json::ArrayIndex left,
                                                        Json::ArrayIndex right) {
        const auto leftPriority = cff::getIntOrDefault(claims[left], "priority", 999);
        const auto rightPriority = cff::getIntOrDefault(claims[right], "priority", 999);
        if (leftPriority != rightPriority) {
            return leftPriority < rightPriority;
        }
        return cff::getIntOrDefault(claims[left], "claimOrder", 999)
            < cff::getIntOrDefault(claims[right], "claimOrder", 999);
    });
    return indexes;
}

Json::Value buildPriorityBoard(const Json::Value &members) {
    Json::Value priorities(Json::arrayValue);
    int priority = 1;
    for (const auto &member : members) {
        if (lowerString(cff::getStringOrDefault(member, "status", "Active")) != "active") {
            continue;
        }
        Json::Value item;
        item["managerEmail"] = cff::getStringOrDefault(member, "email");
        item["role"] = cff::getStringOrDefault(member, "role", "member");
        item["status"] = cff::getStringOrDefault(member, "status", "Active");
        item["priority"] = priority++;
        priorities.append(item);
    }
    return priorities;
}

std::string claimTransactionSummary(const Json::Value &player) {
    return "Claimed " + cff::getStringOrDefault(player, "name");
}

std::string processedTransactionSummary(const Json::Value &player) {
    return "Added " + cff::getStringOrDefault(player, "name");
}

std::string cancelledTransactionSummary() {
    return "Cancelled waiver claim";
}

std::string resetPriorityTransactionSummary() {
    return "Reset waiver priority order";
}

} // namespace cff::league_waiver
