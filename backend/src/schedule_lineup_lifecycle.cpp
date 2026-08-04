#include "schedule_lineup_lifecycle.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace cff::schedule_lineup_lifecycle {
namespace {

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string trim(std::string value) {
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), value.end());
    return value;
}

std::uint64_t fnv1a(const std::string &value) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char ch : value) {
        hash ^= ch;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string hexHash(const std::string &value) {
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << fnv1a(value);
    return out.str();
}

std::vector<std::string> orderedEmails(const Json::Value &members) {
    std::set<std::string> unique;
    if (!members.isArray()) return {};
    for (const auto &member : members) {
        std::string email;
        std::string status{"active"};
        if (member.isString()) {
            email = member.asString();
        } else if (member.isObject()) {
            email = member.get("email", member.get("managerEmail", "")).asString();
            status = lower(member.get("status", "active").asString());
        }
        email = canonicalEmail(std::move(email));
        if (email.empty() || status == "removed" || status == "invited" || status == "pending") continue;
        unique.insert(email);
    }
    return {unique.begin(), unique.end()};
}

std::vector<std::string> rotationForRound(std::vector<std::string> managers, int round) {
    if (managers.size() < 3) return managers;
    for (int step = 0; step < round; ++step) {
        const auto last = managers.back();
        managers.pop_back();
        managers.insert(managers.begin() + 1, last);
    }
    return managers;
}

} // namespace

std::string canonicalEmail(std::string value) {
    return lower(trim(std::move(value)));
}

Json::Value canonicalManagerOrder(const Json::Value &members) {
    Json::Value result(Json::arrayValue);
    for (const auto &email : orderedEmails(members)) result.append(email);
    return result;
}

std::string stableMatchupId(const std::string &leagueId,
                            int season,
                            int week,
                            const std::string &homeManager,
                            const std::string &awayManager) {
    const auto home = canonicalEmail(homeManager);
    const auto away = canonicalEmail(awayManager);
    const auto key = leagueId + "\n" + std::to_string(season) + "\n" + std::to_string(week)
        + "\n" + home + "\n" + away;
    return leagueId + "-s" + std::to_string(season) + "-w" + std::to_string(week)
        + "-" + hexHash(key);
}

Json::Value buildDeterministicSchedule(const Json::Value &members,
                                       const std::string &leagueId,
                                       int season,
                                       int weeks) {
    Json::Value schedule(Json::arrayValue);
    auto managers = orderedEmails(members);
    if (managers.size() < 2 || leagueId.empty() || weeks < 1) return schedule;
    if (managers.size() % 2 == 1) managers.push_back("");

    const int teamCount = static_cast<int>(managers.size());
    const int roundsPerCycle = std::max(1, teamCount - 1);
    for (int week = 1; week <= weeks; ++week) {
        const int zeroBased = week - 1;
        const int cycle = zeroBased / roundsPerCycle;
        const int round = zeroBased % roundsPerCycle;
        const auto rotation = rotationForRound(managers, round);
        for (int pairing = 0; pairing < teamCount / 2; ++pairing) {
            auto home = rotation[static_cast<std::size_t>(pairing)];
            auto away = rotation[static_cast<std::size_t>(teamCount - 1 - pairing)];
            if (home.empty() && away.empty()) continue;
            if (home.empty()) std::swap(home, away);

            const bool flip = ((round + pairing + cycle) % 2) == 1;
            if (flip && !away.empty()) std::swap(home, away);

            Json::Value matchup(Json::objectValue);
            matchup["id"] = stableMatchupId(leagueId, season, week, home, away);
            matchup["leagueId"] = leagueId;
            matchup["season"] = season;
            matchup["week"] = week;
            matchup["homeManager"] = home;
            matchup["awayManager"] = away;
            matchup["homeScore"] = 0.0;
            matchup["awayScore"] = 0.0;
            matchup["status"] = "scheduled";
            matchup["createdAt"] = "";
            matchup["finalizedAt"] = "";
            schedule.append(matchup);
        }
    }
    return schedule;
}

std::string scheduleInputHash(const Json::Value &managerOrder,
                              int season,
                              int weeks) {
    std::ostringstream input;
    input << season << '\n' << weeks;
    if (managerOrder.isArray()) {
        for (const auto &manager : managerOrder) {
            input << '\n' << canonicalEmail(manager.asString());
        }
    }
    return hexHash(input.str());
}

bool expectedVersionMatches(long long currentVersion,
                            const Json::Value &request,
                            bool required) {
    if (!request.isObject() || !request.isMember("expectedVersion")) return !required;
    const auto &value = request["expectedVersion"];
    if (value.isInt64() || value.isUInt64() || value.isInt() || value.isUInt()) {
        return value.asInt64() == currentVersion;
    }
    if (value.isString()) {
        try {
            return std::stoll(value.asString()) == currentVersion;
        } catch (...) {
            return false;
        }
    }
    return false;
}

bool deadlinePassed(const std::string &deadlineIso,
                    const std::string &nowIso) {
    if (deadlineIso.empty() || nowIso.empty()) return false;
    return deadlineIso <= nowIso;
}

bool lockedStatus(const std::string &status) {
    const auto normalized = lower(status);
    return normalized == "locked" || normalized == "finalized";
}

} // namespace cff::schedule_lineup_lifecycle
