#include "schedule_lifecycle.h"

#include <cassert>
#include <iostream>
#include <set>
#include <string>

namespace {

Json::Value member(const std::string &email, const std::string &status = "Active") {
    Json::Value value(Json::objectValue);
    value["email"] = email;
    value["status"] = status;
    return value;
}

void verifyTeamCount(int teamCount) {
    Json::Value members(Json::arrayValue);
    for (int index = teamCount - 1; index >= 0; --index) {
        members.append(member("manager" + std::to_string(index) + "@example.test"));
    }
    const auto first = cff::schedule_lifecycle::buildDeterministicSeason(
        members, "league-test", 2026, 12);
    const auto second = cff::schedule_lifecycle::buildDeterministicSeason(
        members, "league-test", 2026, 12);
    assert(first == second);
    assert(cff::schedule_lifecycle::scheduleFingerprint(first)
        == cff::schedule_lifecycle::scheduleFingerprint(second));

    const auto gamesPerWeek = teamCount / 2;
    assert(static_cast<int>(first.size()) == gamesPerWeek * 12);
    std::set<std::string> ids;
    for (const auto &matchup : first) {
        assert(!matchup["id"].asString().empty());
        assert(ids.insert(matchup["id"].asString()).second);
        assert(matchup["season"].asInt() == 2026);
        assert(matchup["week"].asInt() >= 1);
        assert(matchup["week"].asInt() <= 12);
        assert(!matchup["homeManager"].asString().empty());
    }
}

} // namespace

int main() {
    Json::Value members(Json::arrayValue);
    members.append(member(" B@example.test "));
    members.append(member("a@example.test"));
    members.append(member("removed@example.test", "Removed"));
    members.append(member("A@example.test"));
    const auto managers = cff::schedule_lifecycle::canonicalManagers(members);
    assert(managers.size() == 2);
    assert(managers[0] == "a@example.test");
    assert(managers[1] == "b@example.test");

    const auto keyOne = cff::schedule_lifecycle::stableMatchupKey(
        "a@example.test", "b@example.test");
    const auto keyTwo = cff::schedule_lifecycle::stableMatchupKey(
        "B@example.test", "A@example.test");
    assert(keyOne == keyTwo);
    assert(cff::schedule_lifecycle::stableMatchupId(
        "league", 2026, 1, "a@example.test", "b@example.test")
        == cff::schedule_lifecycle::stableMatchupId(
            "league", 2026, 1, "b@example.test", "a@example.test"));

    verifyTeamCount(4);
    verifyTeamCount(6);
    verifyTeamCount(8);

    assert(cff::schedule_lifecycle::isLineupLockedStatus("locked"));
    assert(cff::schedule_lifecycle::isLineupLockedStatus("FINALIZED"));
    assert(!cff::schedule_lifecycle::isLineupLockedStatus("open"));
    assert(cff::schedule_lifecycle::canUnlockLineup("locked", "unscored", false));
    assert(!cff::schedule_lifecycle::canUnlockLineup("locked", "scored", false));
    assert(!cff::schedule_lifecycle::canUnlockLineup("finalized", "final", true));

    std::cout << "schedule lifecycle rules passed\n";
    return 0;
}
