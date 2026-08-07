#pragma once

#include <json/json.h>

#include <optional>
#include <string>
#include <unordered_map>

namespace cff::league_roster {

bool flexEligible(const std::string &position);

int slotLimit(const Json::Value &rules, const std::string &slot);

bool playerEligibleForSlot(const Json::Value &player, const std::string &slot);

bool validateRosterSlotMove(const Json::Value &player,
                            const Json::Value &roster,
                            const Json::Value &rules,
                            const std::string &playerId,
                            const std::string &slot);

Json::Value lineupAssignmentErrors(const Json::Value &roster,
                                   const Json::Value &rules,
                                   const Json::Value &assignments);

Json::Value lineupErrorsFromCounts(
    const std::string &managerEmail,
    const Json::Value &rules,
    const std::unordered_map<std::string, int> &counts);

int rosterLimitFromRules(const Json::Value &rules);

std::optional<std::string> preferredRosterSlot(
    const Json::Value &player,
    const Json::Value &rules,
    const std::unordered_map<std::string, int> &counts,
    int offset = 0);

} // namespace cff::league_roster
