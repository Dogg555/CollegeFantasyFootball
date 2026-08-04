#pragma once

#include <json/json.h>

#include <string>
#include <vector>

namespace cff::schedule_lifecycle {

std::string canonicalEmail(std::string value);
std::vector<std::string> canonicalManagers(const Json::Value &members);
std::string stableMatchupKey(const std::string &homeManager,
                             const std::string &awayManager);
std::string stableMatchupId(const std::string &leagueId,
                            int season,
                            int week,
                            const std::string &homeManager,
                            const std::string &awayManager);
Json::Value buildDeterministicWeek(const Json::Value &members,
                                   const std::string &leagueId,
                                   int season,
                                   int week);
Json::Value buildDeterministicSeason(const Json::Value &members,
                                     const std::string &leagueId,
                                     int season,
                                     int weeks);
std::string scheduleFingerprint(const Json::Value &matchups);
bool isLineupLockedStatus(const std::string &status);
bool canUnlockLineup(const std::string &lineupStatus,
                     const std::string &scoringStatus,
                     bool matchupFinal);

} // namespace cff::schedule_lifecycle
