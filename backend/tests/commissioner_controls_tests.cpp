#include "commissioner_controls.h"

#include <cassert>
#include <iostream>

using cff::commissioner::Decision;
using cff::commissioner::LeagueState;
using cff::commissioner::MemberAction;
using cff::commissioner::MemberState;

int main() {
    LeagueState league{4, 2, false};
    MemberState pending{true, false, "pending", 0, 0, 0, 0, 0};

    assert(cff::commissioner::evaluateMemberAction(
        MemberAction::approve, league, pending, true).allowed);

    league.activeManagers = 4;
    assert(cff::commissioner::evaluateMemberAction(
        MemberAction::approve, league, pending, true).code == "league_full");

    league.activeManagers = 2;
    league.draftStarted = true;
    assert(cff::commissioner::evaluateMemberAction(
        MemberAction::approve, league, pending, true).code == "draft_started");
    assert(!cff::commissioner::protectedSettingsMutable(league));

    league.draftStarted = false;
    MemberState owner{true, true, "active", 0, 0, 0, 0, 0};
    assert(cff::commissioner::evaluateMemberAction(
        MemberAction::remove, league, owner, true).code == "owner_protected");

    MemberState busy{true, false, "active", 2, 1, 0, 1, 1};
    const Decision busyRemoval = cff::commissioner::evaluateMemberAction(
        MemberAction::remove, league, busy, true);
    assert(!busyRemoval.allowed);
    assert(busyRemoval.blockers.size() == 4);

    MemberState active{true, false, "active", 0, 0, 0, 0, 0};
    assert(cff::commissioner::evaluateMemberAction(
        MemberAction::promote, league, active, false).code == "owner_required");
    assert(cff::commissioner::evaluateMemberAction(
        MemberAction::promote, league, active, true).allowed);
    assert(cff::commissioner::evaluateMemberAction(
        MemberAction::transfer, league, active, true).allowed);

    std::cout << "commissioner control contracts passed\n";
    return 0;
}
