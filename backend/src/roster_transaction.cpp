#include "roster_transaction.h"

#include "league_roster.h"
#include "league_waiver.h"

#include <algorithm>
#include <cctype>

namespace cff::roster_transaction {
namespace {

std::string lowerString(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string playerId(const Json::Value &player) {
    if (!player.isObject()) return "";
    if (player.isMember("id") && player["id"].isString()) {
        return canonicalPlayerId(player["id"].asString());
    }
    if (player.isMember("playerId") && player["playerId"].isString()) {
        return canonicalPlayerId(player["playerId"].asString());
    }
    return "";
}

} // namespace

std::string canonicalPlayerId(std::string value) {
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), value.end());
    return value;
}

bool directAcquisitionAllowed(const Json::Value &waiverRules) {
    return !cff::league_waiver::modeActive(waiverRules);
}

bool rosterContainsPlayer(const Json::Value &roster,
                          const std::string &requestedPlayerId) {
    const auto normalized = canonicalPlayerId(requestedPlayerId);
    if (normalized.empty() || !roster.isArray()) return false;
    for (const auto &player : roster) {
        if (playerId(player) == normalized) return true;
    }
    return false;
}

std::unordered_map<std::string, int> slotCounts(
    const Json::Value &roster,
    const std::string &excludingPlayerId) {
    std::unordered_map<std::string, int> counts;
    const auto excluded = canonicalPlayerId(excludingPlayerId);
    if (!roster.isArray()) return counts;
    for (const auto &player : roster) {
        if (!excluded.empty() && playerId(player) == excluded) continue;
        auto slot = player.isMember("rosterSlot") && player["rosterSlot"].isString()
            ? player["rosterSlot"].asString()
            : "bench";
        slot = lowerString(slot);
        if (slot.empty()) slot = "bench";
        ++counts[slot];
    }
    return counts;
}

std::optional<std::string> destinationSlot(
    const Json::Value &player,
    const Json::Value &roster,
    const Json::Value &rosterRules,
    const std::string &dropPlayerId) {
    const auto counts = slotCounts(roster, dropPlayerId);
    return cff::league_roster::preferredRosterSlot(player, rosterRules, counts);
}

bool expectedVersionMatches(long long currentVersion,
                            const Json::Value &requestBody,
                            bool required) {
    if (!requestBody.isObject() || !requestBody.isMember("expectedVersion")) {
        return !required;
    }
    const auto &value = requestBody["expectedVersion"];
    if (!value.isInt64() && !value.isUInt64() && !value.isInt() && !value.isUInt()) {
        return false;
    }
    return value.asInt64() == currentVersion;
}

} // namespace cff::roster_transaction
