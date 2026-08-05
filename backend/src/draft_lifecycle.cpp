#include "draft_lifecycle.h"

#include "league_schedule.h"

#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cff::draft_lifecycle {
namespace {

int ruleValue(const Json::Value &rules, const char *key, int fallback) {
    if (!rules.isObject() || !rules.isMember(key) || !rules[key].isInt()) {
        return fallback;
    }
    const int value = rules[key].asInt();
    return value >= 0 && value <= 20 ? value : fallback;
}

} // namespace

std::string canonicalEmail(std::string value) {
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), value.end());
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

Json::Value canonicalOrder(const Json::Value &managerEmails) {
    std::vector<std::string> emails;
    std::unordered_set<std::string> seen;
    if (managerEmails.isArray()) {
        for (const auto &entry : managerEmails) {
            const auto raw = entry.isString()
                ? entry.asString()
                : entry.isObject() && entry.isMember("email") && entry["email"].isString()
                    ? entry["email"].asString()
                    : "";
            const auto email = canonicalEmail(raw);
            if (!email.empty() && seen.insert(email).second) emails.push_back(email);
        }
    }
    std::sort(emails.begin(), emails.end());
    Json::Value order(Json::arrayValue);
    for (const auto &email : emails) order.append(email);
    return order;
}

bool orderMatchesManagers(const Json::Value &draftOrder,
                          const Json::Value &managerEmails) {
    const auto managers = canonicalOrder(managerEmails);
    if (!draftOrder.isArray() || draftOrder.size() != managers.size() || draftOrder.empty()) {
        return false;
    }
    std::unordered_set<std::string> expected;
    for (const auto &entry : managers) expected.insert(entry.asString());
    std::unordered_set<std::string> actual;
    for (const auto &entry : draftOrder) {
        if (!entry.isString()) return false;
        const auto email = canonicalEmail(entry.asString());
        if (email.empty() || expected.find(email) == expected.end() || !actual.insert(email).second) {
            return false;
        }
    }
    return actual.size() == expected.size();
}

int rosterSlotsPerManager(const Json::Value &rosterRules) {
    return ruleValue(rosterRules, "qb", 1)
        + ruleValue(rosterRules, "rb", 2)
        + ruleValue(rosterRules, "wr", 2)
        + ruleValue(rosterRules, "te", 1)
        + ruleValue(rosterRules, "flex", 2)
        + ruleValue(rosterRules, "bench", 6);
}

int totalDraftPicks(int managerCount, const Json::Value &rosterRules) {
    if (managerCount <= 0) return 0;
    return managerCount * std::max(1, rosterSlotsPerManager(rosterRules));
}

bool draftCompleteAfterPick(int completedPickNumber,
                            int managerCount,
                            const Json::Value &rosterRules) {
    const int total = totalDraftPicks(managerCount, rosterRules);
    return total > 0 && completedPickNumber >= total;
}

std::string managerForPick(const Json::Value &draftOrder,
                           int pickNumber,
                           const std::string &draftType) {
    return canonicalEmail(cff::league_schedule::currentDraftManager(
        draftOrder,
        std::max(1, pickNumber),
        draftType));
}

bool allManagersReady(const Json::Value &managerEmails,
                      const Json::Value &readiness) {
    const auto managers = canonicalOrder(managerEmails);
    if (managers.empty() || !readiness.isArray()) return false;
    std::unordered_map<std::string, bool> readyByEmail;
    for (const auto &entry : readiness) {
        if (!entry.isObject()) continue;
        const auto email = canonicalEmail(entry.get("email", "").asString());
        if (!email.empty()) readyByEmail[email] = entry.get("ready", false).asBool();
    }
    for (const auto &manager : managers) {
        const auto it = readyByEmail.find(manager.asString());
        if (it == readyByEmail.end() || !it->second) return false;
    }
    return true;
}

} // namespace cff::draft_lifecycle
