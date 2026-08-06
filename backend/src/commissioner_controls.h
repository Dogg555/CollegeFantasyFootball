#pragma once

#include <string>
#include <utility>
#include <vector>

namespace cff::commissioner {

enum class MemberAction {
    approve,
    reject,
    remove,
    promote,
    demote,
    transfer
};

struct LeagueState {
    int teamCount{0};
    int activeManagers{0};
    bool draftStarted{false};
};

struct MemberState {
    bool exists{false};
    bool owner{false};
    std::string status;
    int rosterPlayers{0};
    int draftPicks{0};
    int scheduledMatchups{0};
    int openTrades{0};
    int pendingWaivers{0};
};

struct Decision {
    bool allowed{false};
    std::string code;
    std::string message;
    std::vector<std::string> blockers;
};

Decision evaluateMemberAction(MemberAction action,
                              const LeagueState &league,
                              const MemberState &member,
                              bool actorIsOwner);

bool protectedSettingsMutable(const LeagueState &league);

} // namespace cff::commissioner
