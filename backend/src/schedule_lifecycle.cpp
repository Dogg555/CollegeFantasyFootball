#include "schedule_lifecycle.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <set>
#include <sstream>
#include <utility>

namespace cff::schedule_lifecycle {
namespace {

std::uint64_t fnv1a(const std::string &value) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char ch : value) {
        hash ^= static_cast<std::uint64_t>(ch);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string hashHex(const std::string &value) {
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << fnv1a(value);
    return out.str();
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string stringValue(const Json::Value &value,
                        const char *key,
                        const std::string &fallback = "") {
    return value.isObject() && value.isMember(key) && value[key].isString()
        ? value[key].asString()
        : fallback;
}

} // namespace

std::string canonicalEmail(std::string value) {
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), value.end());
    return lower(std::move(value));
}

std::vector<std::string> canonicalManagers(const Json::Value &members) {
    std::set<std::string> unique;
    for (const auto &member : members) {
        const auto status = lower(stringValue(member, "status", "active"));
        if (status == "removed" || status == "invited" || status == "pending") continue;
        const auto email = canonicalEmail(stringValue(member, "email"));
        if (!email.empty()) unique.insert(email);
    }
    return {unique.begin(), unique.end()};
}

std::string stableMatchupKey(const std::string &homeManager,
                             const std::string &awayManager) {
    auto left = canonicalEmail(homeManager);
    auto right = canonicalEmail(awayManager);
    if (right.empty()) return "bye:" + left;
    if (right < left) std::swap(left, right);
    return left + "|" + right;
}

std::string stableMatchupId(const std::string &leagueId,
                            int season,
                            int week,
                            const std::string &homeManager,
                            const std::string &awayManager) {
    const auto raw = leagueId + "|" + std::to_string(season) + "|" +
        std::to_string(week) + "|" + stableMatchupKey(homeManager, awayManager);
    return leagueId + "-s" + std::to_string(season) + "-w" +
        std::to_string(week) + "-" + hashHex(raw).substr(0, 12);
}

Json::Value buildDeterministicWeek(const Json::Value &members,
                                   const std::string &leagueId,
                                   int season,
                                   int week) {
    auto managers = canonicalManagers(members);
    Json::Value matchups(Json::arrayValue);
    if (managers.empty()) return matchups;
    if (managers.size() % 2 == 1) managers.push_back("");

    const auto teamCount = managers.size();
    const auto roundCount = teamCount > 1 ? teamCount - 1 : 1;
    const auto round = static_cast<std::size_t>((std::max(1, week) - 1) % static_cast<int>(roundCount));
    auto rotated = managers;
    if (teamCount > 2 && round > 0) {
        std::rotate(rotated.begin() + 1,
                    rotated.begin() + 1 + static_cast<std::ptrdiff_t>(round),
                    rotated.end());
    }

    for (std::size_t index = 0; index < teamCount / 2; ++index) {
        auto home = rotated[index];
        auto away = rotated[teamCount - 1 - index];
        if (home.empty() && away.empty()) continue;
        if (home.empty()) std::swap(home, away);
        if (week % 2 == 0 && !away.empty()) std::swap(home, away);

        Json::Value matchup(Json::objectValue);
        matchup["id"] = stableMatchupId(leagueId, season, week, home, away);
        matchup["matchupKey"] = stableMatchupKey(home, away);
        matchup["leagueId"] = leagueId;
        matchup["season"] = season;
        matchup["week"] = std::max(1, week);
        matchup["scheduleSlot"] = static_cast<int>(index + 1);
        matchup["homeManager"] = home;
        matchup["awayManager"] = away;
        matchup["homeScore"] = 0.0;
        matchup["awayScore"] = 0.0;
        matchup["status"] = "scheduled";
        matchups.append(matchup);
    }
    return matchups;
}

Json::Value buildDeterministicSeason(const Json::Value &members,
                                     const std::string &leagueId,
                                     int season,
                                     int weeks) {
    Json::Value schedule(Json::arrayValue);
    const auto count = std::max(1, weeks);
    for (int week = 1; week <= count; ++week) {
        const auto weekly = buildDeterministicWeek(members, leagueId, season, week);
        for (const auto &matchup : weekly) schedule.append(matchup);
    }
    return schedule;
}

std::string scheduleFingerprint(const Json::Value &matchups) {
    std::vector<std::string> rows;
    for (const auto &matchup : matchups) {
        rows.push_back(
            std::to_string(matchup.get("season", 0).asInt()) + ":" +
            std::to_string(matchup.get("week", 0).asInt()) + ":" +
            matchup.get("matchupKey", "").asString() + ":" +
            canonicalEmail(matchup.get("homeManager", "").asString()) + ":" +
            canonicalEmail(matchup.get("awayManager", "").asString()));
    }
    std::sort(rows.begin(), rows.end());
    std::ostringstream joined;
    for (const auto &row : rows) joined << row << '\n';
    return hashHex(joined.str());
}

bool isLineupLockedStatus(const std::string &status) {
    const auto normalized = lower(status);
    return normalized == "locked" || normalized == "finalized";
}

bool canUnlockLineup(const std::string &lineupStatus,
                     const std::string &scoringStatus,
                     bool matchupFinal) {
    if (!isLineupLockedStatus(lineupStatus)) return false;
    if (lower(lineupStatus) == "finalized" || matchupFinal) return false;
    return lower(scoringStatus) == "unscored";
}

} // namespace cff::schedule_lifecycle
