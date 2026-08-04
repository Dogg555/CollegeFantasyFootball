#pragma once

#include <json/json.h>
#include <string>

namespace cff::schedule_lineup {

std::string canonicalManager(std::string value);
std::string matchupId(const std::string &leagueId, int season, int week,
                      const std::string &homeManager,
                      const std::string &awayManager);
Json::Value deterministicSchedule(const Json::Value &members,
                                  const std::string &leagueId,
                                  int season,
                                  int weeks);
bool sameScheduleIdentity(const Json::Value &left, const Json::Value &right);
bool lineupMutationAllowed(const Json::Value &weekState,
                           const std::string &nowIso,
                           bool commissionerOverride = false);
bool canUnlockWeek(const Json::Value &weekState);
long long nextVersion(long long current);

} // namespace cff::schedule_lineup
