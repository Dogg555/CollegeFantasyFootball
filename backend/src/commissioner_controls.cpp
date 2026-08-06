#include "commissioner_controls.h"

#include <algorithm>
#include <cctype>

namespace cff::commissioner {
namespace {

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

Decision deny(std::string code, std::string message) {
    Decision decision;
    decision.code = std::move(code);
    decision.message = std::move(message);
    return decision;
}

Decision allow() {
    Decision decision;
    decision.allowed = true;
    decision.code = "ok";
    return decision;
}

} // namespace

Decision evaluateMemberAction(MemberAction action,
                              const LeagueState &league,
                              const MemberState &member,
                              bool actorIsOwner) {
    if (!member.exists) {
        return deny("member_not_found", "The selected manager is not part of this league.");
    }

    const auto status = lower(member.status);

    if (action == MemberAction::approve) {
        if (!(status == "pending" || status == "invited")) {
            return deny("member_not_pending", "Only invited or pending managers can be approved.");
        }
        if (league.draftStarted) {
            return deny("draft_started", "Managers cannot be added after the draft has started.");
        }
        if (league.activeManagers >= league.teamCount) {
            return deny("league_full", "The league already has the configured number of active teams.");
        }
        return allow();
    }

    if (action == MemberAction::reject) {
        if (member.owner) {
            return deny("owner_protected", "The league owner cannot be rejected.");
        }
        if (!(status == "pending" || status == "invited")) {
            return deny("member_not_pending", "Only invited or pending managers can be rejected.");
        }
        return allow();
    }

    if (action == MemberAction::remove) {
        if (member.owner) {
            return deny("owner_protected", "Transfer ownership before removing the league owner.");
        }
        if (league.draftStarted) {
            return deny("draft_started", "Managers cannot be removed after the draft has started. Reassignment is required.");
        }

        Decision decision = allow();
        if (member.rosterPlayers > 0) decision.blockers.push_back("roster_players");
        if (member.draftPicks > 0) decision.blockers.push_back("draft_picks");
        if (member.scheduledMatchups > 0) decision.blockers.push_back("scheduled_matchups");
        if (member.openTrades > 0) decision.blockers.push_back("open_trades");
        if (member.pendingWaivers > 0) decision.blockers.push_back("pending_waivers");
        if (!decision.blockers.empty()) {
            decision.allowed = false;
            decision.code = "manager_has_competition_data";
            decision.message = "Resolve the manager's fantasy activity before removing them.";
        }
        return decision;
    }

    if (action == MemberAction::promote || action == MemberAction::demote) {
        if (!actorIsOwner) {
            return deny("owner_required", "Only the league owner can change commissioner access.");
        }
        if (member.owner) {
            return deny("owner_protected", "The league owner must remain a commissioner.");
        }
        if (status != "active") {
            return deny("active_manager_required", "Commissioner access can only be changed for active managers.");
        }
        return allow();
    }

    if (action == MemberAction::transfer) {
        if (!actorIsOwner) {
            return deny("owner_required", "Only the league owner can transfer ownership.");
        }
        if (member.owner) {
            return deny("already_owner", "That manager already owns the league.");
        }
        if (status != "active") {
            return deny("active_manager_required", "Ownership can only be transferred to an active manager.");
        }
        return allow();
    }

    return deny("unsupported_action", "Unsupported commissioner action.");
}

bool protectedSettingsMutable(const LeagueState &league) {
    return !league.draftStarted;
}

} // namespace cff::commissioner
