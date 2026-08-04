#pragma once

#include <json/json.h>

#include <string>
#include <vector>

namespace cff::waiver_lifecycle {

std::string canonicalEmail(std::string value);
std::string canonicalPlayerId(std::string value);
long long normalizedVersion(const Json::Value &value, long long fallback = 0);
bool expectedVersionMatches(long long currentVersion,
                            const Json::Value &body,
                            bool required = true);
bool validClaimReorder(const Json::Value &pendingClaims,
                       const Json::Value &claimIds,
                       const std::string &managerEmail);
Json::Value canonicalPriorityBoard(const Json::Value &members);
void moveManagerToBack(Json::Value &priorityBoard,
                       const std::string &managerEmail);
std::vector<Json::ArrayIndex> orderedClaimIndexes(const Json::Value &claims,
                                                  const Json::Value &priorityBoard);

} // namespace cff::waiver_lifecycle
