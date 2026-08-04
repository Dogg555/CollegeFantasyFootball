#include "schedule_lineup_lifecycle.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <iomanip>
#include <sstream>
#include <vector>

namespace cff::schedule_lineup {
namespace {
std::string stringValue(const Json::Value &value, const char *key) {
    return value.isObject() && value.isMember(key) && value[key].isString()
        ? value[key].asString() : "";
}

std::string stableHash(const std::string &value) {
    const auto hash = std::hash<std::string>{}(value);
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}
}

std::string canonicalManager(std::string value) {
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

std::string matchupId(const std::string &leagueId, int season, int week,
                      const std::string &homeManager,
                      const std::string &awayManager) {
    const auto material = leagueId + "|" + std::to_string(season) + "|" +
                          std::to_string(week) + "|" + canonicalManager(homeManager) +
                          "|" + canonicalManager(awayManager);
    return leagueId + "-" + std::to_string(season) + "-w" +
           std::to_string(week) + "-" + stableHash(material);
}

Json::Value deterministicSchedule(const Json::Value &members,
                                  const std::string &leagueId,
                                  int season,
                                  int weeks) {
    std::vector<std::string> managers;
    for (const auto &member : members) {
        const auto status = canonicalManager(stringValue(member, "status"));
        const auto email = canonicalManager(stringValue(member, "email"));
        if (!email.empty() && status != "removed" && status != "invited" && status != "pending") {
            managers.push_back(email);
        }
    }
    std::sort(managers.begin(), managers.end());
    managers.erase(std::unique(managers.begin(), managers.end()), managers.end());
    if (managers.size() % 2 == 1) managers.push_back("");

    Json::Value schedule(Json::arrayValue);
    if (managers.empty()) return schedule;
    const auto teamCount = managers.size();
    const auto rounds = teamCount > 1 ? static_cast<int>(teamCount - 1) : 1;
    weeks = std::max(1, weeks);

    for (int week = 1; week <= weeks; ++week) {
        auto rotated = managers;
        const int round = (week - 1) % rounds;
        if (teamCount > 2) {
            std::rotate(rotated.begin() + 1,
                        rotated.begin() + 1 + round,
                        rotated.end());
        }
        for (std::size_t index = 0; index < teamCount / 2; ++index) {
            auto home = rotated[index];
            auto away = rotated[teamCount - 1 - index];
            if (home.empty() && away.empty()) continue;
            if (home.empty()) std::swap(home, away);
            if (week % 2 == 0 && !away.empty()) std::swap(home, away);
            Json::Value matchup(Json::objectValue);
            matchup["id"] = matchupId(leagueId, season, week, home, away);
            matchup["leagueId"] = leagueId;
            matchup["season"] = season;
            matchup["week"] = week;
            matchup["homeManager"] = home;
            matchup["awayManager"] = away;
            matchup["status"] = "scheduled";
            schedule.append(matchup);
        }
    }
    return schedule;
}

bool sameScheduleIdentity(const Json::Value &left, const Json::Value &right) {
    if (!left.isArray() || !right.isArray() || left.size() != right.size()) return false;
    for (Json::ArrayIndex index = 0; index < left.size(); ++index) {
        if (stringValue(left[index], "id") != stringValue(right[index], "id")) return false;
        if (left[index].get("week", 0).asInt() != right[index].get("week", 0).asInt()) return false;
        if (canonicalManager(stringValue(left[index], "homeManager")) !=
            canonicalManager(stringValue(right[index], "homeManager"))) return false;
        if (canonicalManager(stringValue(left[index], "awayManager")) !=
            canonicalManager(stringValue(right[index], "awayManager"))) return false;
    }
    return true;
}

bool lineupMutationAllowed(const Json::Value &weekState,
                           const std::string &nowIso,
                           bool commissionerOverride) {
    const auto status = canonicalManager(stringValue(weekState, "status"));
    if (status == "final" || status == "finalized") return false;
    if (commissionerOverride) return true;
    if (weekState.get("locked", false).asBool()) return false;
    const auto deadline = stringValue(weekState, "lineupDeadline");
    return deadline.empty() || nowIso.empty() || nowIso < deadline;
}

bool canUnlockWeek(const Json::Value &weekState) {
    const auto status = canonicalManager(stringValue(weekState, "status"));
    return status != "final" && status != "finalized";
}

long long nextVersion(long long current) {
    return std::max<long long>(0, current) + 1;
}

} // namespace cff::schedule_lineup
