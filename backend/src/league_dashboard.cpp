#include "league_dashboard.h"

#include <algorithm>
#include <cctype>

namespace cff::league_dashboard {
namespace {

std::string canonicalSlot(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool draftActive(const std::string &status) {
    const auto value = canonicalSlot(status);
    return value == "open" || value == "paused";
}

bool draftComplete(const std::string &status) {
    return canonicalSlot(status) == "complete";
}

} // namespace

std::vector<LineupWarning> missingStarterWarnings(
    const std::map<std::string, int> &requiredSlots,
    const std::map<std::string, int> &assignedSlots,
    bool isDraftComplete) {
    std::vector<LineupWarning> warnings;
    if (!isDraftComplete) return warnings;

    for (const auto &[rawSlot, required] : requiredSlots) {
        const auto slot = canonicalSlot(rawSlot);
        if (slot == "bench" || required <= 0) continue;
        const auto assigned = assignedSlots.find(slot);
        const auto assignedCount = assigned == assignedSlots.end() ? 0 : assigned->second;
        if (assignedCount < required) {
            warnings.push_back({slot, required - assignedCount});
        }
    }
    return warnings;
}

NextAction chooseNextAction(const DashboardSignals &signals) {
    if (!draftComplete(signals.draftStatus)) {
        if (signals.commissioner
            && signals.teamCount > 0
            && signals.activeManagers < signals.teamCount
            && !draftActive(signals.draftStatus)) {
            return {
                "invite_managers",
                "Invite managers",
                "league.html#managers",
                "Fill the remaining league slots before starting the draft."
            };
        }
        return {
            "open_draft",
            draftActive(signals.draftStatus) ? "Enter draft room" : "Prepare the draft",
            "draft.html",
            draftActive(signals.draftStatus)
                ? "The draft is active. Rejoin the shared draft room."
                : "Review the draft order and open the lobby when the league is ready."
        };
    }

    if (signals.hasLineupWarnings) {
        return {
            "fix_lineup",
            "Fix your lineup",
            "league.html#team",
            "One or more required starter slots are empty."
        };
    }

    if (signals.actionRequiredTrades > 0) {
        return {
            "review_trade",
            "Review trade offer",
            "league.html#trades",
            "A trade is waiting for your response or commissioner decision."
        };
    }

    if (signals.pendingWaivers > 0) {
        return {
            "review_waivers",
            "Review waiver claims",
            "league.html#waivers",
            "You have pending waiver activity in this league."
        };
    }

    if (signals.hasCurrentMatchup) {
        return {
            "view_matchup",
            "View current matchup",
            "league.html#scoreboard",
            "Check the current scoring week and matchup status."
        };
    }

    if (!signals.hasRoster) {
        return {
            "browse_players",
            "Browse players",
            "players.html",
            "Start building your team from the player catalog."
        };
    }

    return {
        "view_team",
        "Review your team",
        "league.html#team",
        "Check your roster and upcoming lineup before the next scoring period."
    };
}

} // namespace cff::league_dashboard
