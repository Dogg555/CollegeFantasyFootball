#pragma once

#include <json/json.h>

#include <string>

namespace cff::scoring_lifecycle {

std::string canonicalEmail(std::string value);
std::string canonicalStatToken(std::string value);

long long normalizedVersion(const Json::Value &value, long long fallback = -1);
bool expectedVersionMatches(long long currentVersion,
                            const Json::Value &body,
                            bool required);

bool finalizedStatus(const std::string &status);
bool scoredStatus(const std::string &status);

double fantasyPointsForStat(const Json::Value &settings,
                            const std::string &category,
                            const std::string &statName,
                            double value);

Json::Value standingsFromFinalMatchups(const Json::Value &members,
                                       const Json::Value &matchups,
                                       int season = 0);

} // namespace cff::scoring_lifecycle
