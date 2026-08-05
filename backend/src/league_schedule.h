#pragma once

#include <functional>
#include <string>

#include <json/json.h>
#ifdef CFF_HAS_POSTGRES
#include <postgresql/libpq-fe.h>
#endif

namespace cff::handlers {

#ifdef CFF_HAS_POSTGRES
int activeMemberCountForLeague(PGconn *connection, const std::string &leagueId);
#endif

} // namespace cff::handlers

namespace cff::league_schedule {

Json::Value activeMembers(const Json::Value &members);

Json::Value buildMatchups(
    const Json::Value &members,
    const std::string &leagueId,
    int week,
    const std::function<double(const std::string &)> &scoreForManager
);

Json::Value buildSeasonSchedule(
    const Json::Value &members,
    const std::string &leagueId,
    int weeks,
    const std::function<double(const std::string &)> &scoreForManager
);

std::string currentDraftManager(
    const Json::Value &draftOrder,
    int currentPick,
    const std::string &draftType = "snake"
);

} // namespace cff::league_schedule
