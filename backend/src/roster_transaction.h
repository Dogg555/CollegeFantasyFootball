#pragma once

#include <json/json.h>

#include <optional>
#include <string>
#include <unordered_map>

namespace cff::roster_transaction {

std::string canonicalPlayerId(std::string value);

bool directAcquisitionAllowed(const Json::Value &waiverRules);

bool rosterContainsPlayer(const Json::Value &roster,
                          const std::string &playerId);

std::unordered_map<std::string, int> slotCounts(
    const Json::Value &roster,
    const std::string &excludingPlayerId = "");

std::optional<std::string> destinationSlot(
    const Json::Value &player,
    const Json::Value &roster,
    const Json::Value &rosterRules,
    const std::string &dropPlayerId = "");

bool expectedVersionMatches(long long currentVersion,
                            const Json::Value &requestBody,
                            bool required);

} // namespace cff::roster_transaction
