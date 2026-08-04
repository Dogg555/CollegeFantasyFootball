#include "league_roster.h"

#include <algorithm>
#include <cctype>

#include "json_utils.h"

namespace cff::league_roster {

namespace {

std::string lowerString(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string upperString(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return value;
}

int countFor(const std::unordered_map<std::string, int> &counts,
             const std::string &slot) {
    const auto found = counts.find(slot);
    return found == counts.end() ? 0 : found->second;
}

} // namespace

bool flexEligible(const std::string &position) {
    const auto normalized = lowerString(position);
    return normalized == "rb" || normalized == "wr" || normalized == "te";
}

int slotLimit(const Json::Value &rules, const std::string &slot) {
    return cff::getIntOrDefault(rules, lowerString(slot), 0);
}

bool playerEligibleForSlot(const Json::Value &player, const std::string &slot) {
    const auto normalizedSlot = lowerString(slot);
    const auto position = lowerString(cff::getStringOrDefault(player, "position", "bench"));
    if (normalizedSlot == "bench") {
        return true;
    }
    if (normalizedSlot == "flex") {
        return flexEligible(position);
    }
    return normalizedSlot == position;
}

bool validateRosterSlotMove(const Json::Value &player,
                            const Json::Value &roster,
                            const Json::Value &rules,
                            const std::string &playerId,
                            const std::string &slot) {
    const auto normalizedSlot = lowerString(slot);
    if (normalizedSlot != "qb" && normalizedSlot != "rb" && normalizedSlot != "wr"
        && normalizedSlot != "te" && normalizedSlot != "flex"
        && normalizedSlot != "bench") {
        return false;
    }
    if (!playerEligibleForSlot(player, normalizedSlot)) {
        return false;
    }

    int occupied = 0;
    for (const auto &item : roster) {
        if (cff::getStringOrDefault(item, "id") == playerId) {
            continue;
        }
        if (lowerString(cff::getStringOrDefault(item, "rosterSlot", "bench"))
            == normalizedSlot) {
            ++occupied;
        }
    }
    return occupied < slotLimit(rules, normalizedSlot);
}

Json::Value lineupErrorsFromCounts(
    const std::string &managerEmail,
    const Json::Value &rules,
    const std::unordered_map<std::string, int> &counts) {
    Json::Value errors(Json::arrayValue);
    for (const auto &slot : {"qb", "rb", "wr", "te", "flex"}) {
        const auto required = slotLimit(rules, slot);
        const auto filled = countFor(counts, slot);
        if (filled < required) {
            Json::Value error;
            error["managerEmail"] = managerEmail;
            error["slot"] = slot;
            error["message"] = "Missing " + std::to_string(required - filled)
                + " " + upperString(slot) + " starter(s)";
            errors.append(error);
        }
        if (filled > required) {
            Json::Value error;
            error["managerEmail"] = managerEmail;
            error["slot"] = slot;
            error["message"] = "Too many " + upperString(slot) + " starter(s)";
            errors.append(error);
        }
    }
    return errors;
}

int rosterLimitFromRules(const Json::Value &rules) {
    if (!rules.isObject()) {
        return 14;
    }
    return cff::getIntOrDefault(rules, "qb", 1)
        + cff::getIntOrDefault(rules, "rb", 2)
        + cff::getIntOrDefault(rules, "wr", 2)
        + cff::getIntOrDefault(rules, "te", 1)
        + cff::getIntOrDefault(rules, "flex", 2)
        + cff::getIntOrDefault(rules, "bench", 6);
}

std::optional<std::string> preferredRosterSlot(
    const Json::Value &player,
    const Json::Value &rules,
    const std::unordered_map<std::string, int> &counts,
    int offset) {
    const auto position = lowerString(cff::getStringOrDefault(player, "position", "flex"));
    const auto naturalLimit = cff::getIntOrDefault(rules, position, 0);
    if (naturalLimit > 0 && countFor(counts, position) + offset < naturalLimit) {
        return position;
    }
    if (flexEligible(position)
        && countFor(counts, "flex") + offset < cff::getIntOrDefault(rules, "flex", 0)) {
        return "flex";
    }
    if (countFor(counts, "bench") + offset < cff::getIntOrDefault(rules, "bench", 0)) {
        return "bench";
    }
    return std::nullopt;
}

} // namespace cff::league_roster
