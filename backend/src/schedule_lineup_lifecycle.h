#pragma once

#include <json/json.h>

#include <string>

namespace cff::schedule_lineup_lifecycle {

std::string canonicalEmail(std::string value);

Json::Value canonicalManagerOrder(const Json::Value &members);

std::string stableMatchupId(const std::string &leagueId,
                            int season,
                            int week,
                            const std::string &homeManager,
                            const std::string &awayManager);

Json::Value buildDeterministicSchedule(const Json::Value &members,
                                       const std::string &leagueId,
                                       int season,
                                       int weeks);

std::string scheduleInputHash(const Json::Value &managerOrder,
                              int season,
                              int weeks);

bool expectedVersionMatches(long long currentVersion,
                            const Json::Value &request,
                            bool required = true);

bool deadlinePassed(const std::string &deadlineIso,
                    const std::string &nowIso);

bool lockedStatus(const std::string &status);

} // namespace cff::schedule_lineup_lifecycle
