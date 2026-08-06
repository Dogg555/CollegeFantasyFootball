#include "league_schedule.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

#ifdef CFF_HAS_POSTGRES
namespace cff::handlers {

int activeMemberCountForLeague(PGconn *connection, const std::string &leagueId) {
    if (!connection || leagueId.empty()) {
        return 0;
    }

    const char *values[] = {leagueId.c_str()};
    PGresult *result = PQexecParams(
        connection,
        "SELECT COUNT(*) FROM league_members WHERE league_id = $1 AND status = 'active'",
        1,
        nullptr,
        values,
        nullptr,
        nullptr,
        0
    );
    if (!result) {
        return 0;
    }

    int count = 0;
    if (PQresultStatus(result) == PGRES_TUPLES_OK
        && PQntuples(result) > 0
        && !PQgetisnull(result, 0, 0)) {
        count = std::atoi(PQgetvalue(result, 0, 0));
    }
    PQclear(result);
    return count;
}

} // namespace cff::handlers
#endif

namespace cff::league_schedule {
namespace {

std::string lowerString(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string stringValue(
    const Json::Value &value,
    const char *key,
    const std::string &fallback = ""
) {
    if (!value.isObject() || !value.isMember(key) || !value[key].isString()) {
        return fallback;
    }
    return value[key].asString();
}

} // namespace

Json::Value activeMembers(const Json::Value &members) {
    Json::Value active(Json::arrayValue);
    for (const auto &member : members) {
        const auto status = lowerString(stringValue(member, "status", "Active"));
        if (status == "active" && status != "Removed" && status != "removed") {
            active.append(member);
        }
    }
    return active;
}

Json::Value buildMatchups(
    const Json::Value &members,
    const std::string &leagueId,
    int week,
    const std::function<double(const std::string &)> &scoreForManager
) {
    Json::Value matchups(Json::arrayValue);
    const auto managers = activeMembers(members);
    std::vector<std::string> emails;
    emails.reserve(managers.size());
    for (const auto &manager : managers) {
        const auto email = stringValue(manager, "email");
        if (!email.empty()) {
            emails.push_back(email);
        }
    }
    if (emails.empty()) {
        return matchups;
    }

    const bool hasBye = emails.size() % 2 == 1;
    if (hasBye) {
        emails.push_back("");
    }

    const auto teamCount = emails.size();
    const auto roundCount = teamCount > 1 ? teamCount - 1 : 1;
    const auto round = (std::max(1, week) - 1) % roundCount;
    std::vector<std::string> rotated = emails;
    if (teamCount > 2) {
        std::rotate(rotated.begin() + 1, rotated.begin() + 1 + round, rotated.end());
    }

    for (std::size_t i = 0; i < teamCount / 2; ++i) {
        auto home = rotated[i];
        auto away = rotated[teamCount - 1 - i];
        if (home.empty() && away.empty()) {
            continue;
        }
        if (home.empty()) {
            std::swap(home, away);
        }
        if (week % 2 == 0 && !away.empty()) {
            std::swap(home, away);
        }

        Json::Value matchup;
        matchup["id"] = leagueId + "-week-" + std::to_string(week) + "-" + std::to_string(i + 1);
        matchup["leagueId"] = leagueId;
        matchup["week"] = week;
        matchup["homeManager"] = home;
        matchup["awayManager"] = away;
        matchup["homeScore"] = scoreForManager(home);
        matchup["awayScore"] = away.empty() ? 0.0 : scoreForManager(away);
        matchup["status"] = "scheduled";
        matchup["createdAt"] = "";
        matchups.append(matchup);
    }
    return matchups;
}

Json::Value buildSeasonSchedule(
    const Json::Value &members,
    const std::string &leagueId,
    int weeks,
    const std::function<double(const std::string &)> &scoreForManager
) {
    Json::Value schedule(Json::arrayValue);
    for (int week = 1; week <= weeks; ++week) {
        const auto weekly = buildMatchups(members, leagueId, week, scoreForManager);
        for (const auto &matchup : weekly) {
            schedule.append(matchup);
        }
    }
    return schedule;
}

std::string currentDraftManager(
    const Json::Value &draftOrder,
    int currentPick,
    const std::string &draftType
) {
    if (!draftOrder.isArray() || draftOrder.empty()) {
        return "";
    }

    const auto orderSize = static_cast<int>(draftOrder.size());
    const auto zeroBasedPick = std::max(1, currentPick) - 1;
    const auto round = zeroBasedPick / orderSize;
    auto offset = zeroBasedPick % orderSize;
    if (lowerString(draftType) == "snake" && round % 2 == 1) {
        offset = orderSize - 1 - offset;
    }

    const auto index = static_cast<Json::ArrayIndex>(offset);
    return draftOrder[index].isString() ? draftOrder[index].asString() : "";
}

} // namespace cff::league_schedule
