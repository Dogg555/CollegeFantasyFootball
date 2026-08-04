#pragma once

#include <json/json.h>

#include <cstdint>
#include <string>

namespace cff::stat_ingestion_lifecycle {

std::string canonicalToken(std::string value);
std::string statRecordKey(const Json::Value &record);
std::string statSourceHash(const Json::Value &record);
int retryDelaySeconds(int attempt,
                      int retryAfterSeconds = 0,
                      int maximumSeconds = 900);
bool retryableProviderFailure(int statusCode, bool networkFailure = false);
bool sourceFresh(long long ageSeconds, int staleAfterSeconds);
std::string recalculationStatus(const std::string &scoringStatus);
std::string recalculationReason(const std::string &scoringStatus);

} // namespace cff::stat_ingestion_lifecycle
