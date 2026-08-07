#include "league_roster.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include "json_utils.h"

namespace cff::league_roster {

namespace {

const std::unordered_set<std::string> kRosterSlots{
    "qb", "rb", "wr", "te", "flex", "k", "def", "bench"
};

std::string lowerString(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string trimString(std::string value) {
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), value.end());
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

std::optional<std::string> stringMember(const Json::Value &value, const char *key) {
    if (!value.isObject() || !value.isMember(key) || !value[key].isString()) {
        return std::nullopt;
    }
    auto result = trimString(value[key].asString());
    return result.empty() ? std::nullopt : std::optional<std::string>{std::move(result)};
}

std::string playerId(const Json::Value &value) {
    const auto primary = stringMember(value, "playerId");
    if (primary) return *primary;
    const auto legacy = stringMember(value, "id");
    return legacy.value_or("");
}

Json::Value lineupError(const std::string &code,
                        const std::string &message,
                        const std::string &player = "",
                        const std::string &slot = "") {
    Json::Value error(Json::objectValue);
    error["code"] = code;
    error["message"] = message;
    if (!player.empty()) error["playerId"] = player;
    if (!slot.empty()) error["slot"] = slot;
    return error;
}

} // namespace

bool flexEligible(const std::string &position) {
    const auto normalized = lowerString(position);
    return normalized == "rb" || normalized == "wr" || normalized == "te";
}

int slotLimit(const Json::Value &rules, const std::string &slot) {
    return cff::getIntOrDefault(rules, slot, 0);
}

bool playerEligibleForSlot(const Json::Value &player, const std::string &slot) {
    const auto position = lowerString(cff::getStringOrDefault(player, "position", "bench"));
    if (slot == "bench") return true;
    if (slot == "flex") return flexEligible(position);
    return slot == position;
}

bool validateRosterSlotMove(const Json::Value &player,
                            const Json::Value &roster,
                            const Json::Value &rules,
                            const std::string &requestedPlayerId,
                            const std::string &slot) {
    if (!kRosterSlots.count(slot)) return false;
    if (!playerEligibleForSlot(player, slot)) return false;
    // A single staged move may temporarily overfill the bench while the user
    // opens a starter slot. The final whole-lineup save validates bench size.
    if (slot == "bench") return true;

    int occupied = 0;
    for (const auto &item : roster) {
        if (cff::getStringOrDefault(item, "id") == requestedPlayerId) continue;
        if (lowerString(cff::getStringOrDefault(item, "rosterSlot", "bench")) == slot) ++occupied;
    }
    return occupied < slotLimit(rules, slot);
}

Json::Value lineupAssignmentErrors(const Json::Value &roster,
                                   const Json::Value &rules,
                                   const Json::Value &assignments) {
    Json::Value errors(Json::arrayValue);
    if (!roster.isArray()) {
        errors.append(lineupError("roster_unavailable", "The current roster could not be loaded."));
        return errors;
    }
    if (!assignments.isArray()) {
        errors.append(lineupError("lineup_assignments_required", "A complete lineup assignment list is required."));
        return errors;
    }

    std::unordered_map<std::string, Json::Value> rosterByPlayer;
    for (const auto &player : roster) {
        const auto id = playerId(player);
        if (!id.empty()) rosterByPlayer.emplace(id, player);
    }

    std::unordered_set<std::string> assignedPlayers;
    std::unordered_map<std::string, int> counts;
    for (const auto &assignment : assignments) {
        if (!assignment.isObject()) {
            errors.append(lineupError(
                "invalid_lineup_assignment",
                "Every lineup assignment must be an object with string playerId and slot fields."));
            continue;
        }

        const auto id = playerId(assignment);
        if (id.empty()) {
            errors.append(lineupError(
                "lineup_player_required",
                "Every lineup assignment must include a string player ID."));
            continue;
        }

        const auto slotValue = stringMember(assignment, "slot");
        if (!slotValue) {
            errors.append(lineupError(
                "invalid_lineup_slot",
                "Every lineup assignment must include a string roster slot.",
                id));
            continue;
        }
        const auto slot = lowerString(*slotValue);

        if (!assignedPlayers.insert(id).second) {
            errors.append(lineupError("duplicate_lineup_player", "A player cannot appear more than once in a lineup.", id));
            continue;
        }
        const auto player = rosterByPlayer.find(id);
        if (player == rosterByPlayer.end()) {
            errors.append(lineupError("lineup_player_not_rostered", "The lineup contains a player who is not on this roster.", id));
            continue;
        }
        if (!kRosterSlots.count(slot)) {
            errors.append(lineupError("invalid_lineup_slot", "The lineup contains an unsupported roster slot.", id, slot));
            continue;
        }
        if (!playerEligibleForSlot(player->second, slot)) {
            const auto name = stringMember(player->second, "name").value_or(id);
            errors.append(lineupError(
                "lineup_player_ineligible",
                name + " is not eligible for " + upperString(slot) + ".",
                id,
                slot));
            continue;
        }
        ++counts[slot];
    }

    for (const auto &[id, player] : rosterByPlayer) {
        (void)player;
        if (!assignedPlayers.count(id)) {
            errors.append(lineupError(
                "lineup_player_missing",
                "Every rostered player must be included in the lineup save.",
                id));
        }
    }

    if (assignments.size() != roster.size()) {
        errors.append(lineupError(
            "lineup_assignment_count_mismatch",
            "The lineup must assign every rostered player exactly once."));
    }

    for (const auto &slot : kRosterSlots) {
        const auto limit = slotLimit(rules, slot);
        const auto filled = countFor(counts, slot);
        if (filled > limit) {
            errors.append(lineupError(
                "lineup_slot_overfilled",
                "Too many players are assigned to " + upperString(slot) + ".",
                "",
                slot));
        }
    }
    return errors;
}

Json::Value lineupErrorsFromCounts(
    const std::string &managerEmail,
    const Json::Value &rules,
    const std::unordered_map<std::string, int> &counts) {
    Json::Value errors(Json::arrayValue);
    for (const auto &slot : {"qb", "rb", "wr", "te", "flex", "k", "def"}) {
        const auto required = slotLimit(rules, slot);
        const auto filled = countFor(counts, slot);
        // Empty starter slots are legal and score zero points.
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
    if (!rules.isObject()) return 14;
    return cff::getIntOrDefault(rules, "qb", 1)
        + cff::getIntOrDefault(rules, "rb", 2)
        + cff::getIntOrDefault(rules, "wr", 2)
        + cff::getIntOrDefault(rules, "te", 1)
        + cff::getIntOrDefault(rules, "flex", 2)
        + cff::getIntOrDefault(rules, "k", 0)
        + cff::getIntOrDefault(rules, "def", 0)
        + cff::getIntOrDefault(rules, "bench", 6);
}

std::optional<std::string> preferredRosterSlot(
    const Json::Value &player,
    const Json::Value &rules,
    const std::unordered_map<std::string, int> &counts,
    int offset) {
    const auto position = lowerString(cff::getStringOrDefault(player, "position", "flex"));
    const auto naturalLimit = cff::getIntOrDefault(rules, position, 0);
    if (naturalLimit > 0 && countFor(counts, position) + offset < naturalLimit) return position;
    if (flexEligible(position)
        && countFor(counts, "flex") + offset < cff::getIntOrDefault(rules, "flex", 0)) {
        return "flex";
    }
    if (countFor(counts, "bench") + offset < cff::getIntOrDefault(rules, "bench", 0)) return "bench";
    return std::nullopt;
}

} // namespace cff::league_roster
