#include "schedule_lineup_lifecycle.h"

#include <cassert>
#include <iostream>
#include <set>

namespace {
Json::Value member(const std::string &email, const std::string &status = "active") {
    Json::Value value(Json::objectValue);
    value["email"] = email;
    value["status"] = status;
    return value;
}
}

int main() {
    Json::Value members(Json::arrayValue);
    members.append(member("d@example.com"));
    members.append(member("b@example.com"));
    members.append(member("a@example.com"));
    members.append(member("c@example.com"));
    members.append(member("ignored@example.com", "invited"));

    const auto first = cff::schedule_lineup::deterministicSchedule(members, "league-1", 2026, 6);
    const auto second = cff::schedule_lineup::deterministicSchedule(members, "league-1", 2026, 6);
    assert(first.size() == 12);
    assert(cff::schedule_lineup::sameScheduleIdentity(first, second));

    std::set<std::string> ids;
    for (const auto &matchup : first) {
        assert(ids.insert(matchup["id"].asString()).second);
        assert(matchup["season"].asInt() == 2026);
        assert(matchup["week"].asInt() >= 1);
        assert(!matchup["homeManager"].asString().empty());
    }

    Json::Value reordered(Json::arrayValue);
    reordered.append(member("c@example.com"));
    reordered.append(member("a@example.com"));
    reordered.append(member("d@example.com"));
    reordered.append(member("b@example.com"));
    const auto reorderedSchedule = cff::schedule_lineup::deterministicSchedule(reordered, "league-1", 2026, 6);
    assert(cff::schedule_lineup::sameScheduleIdentity(first, reorderedSchedule));

    Json::Value open(Json::objectValue);
    open["status"] = "open";
    open["locked"] = false;
    open["lineupDeadline"] = "2026-09-01T00:00:00Z";
    assert(cff::schedule_lineup::lineupMutationAllowed(open, "2026-08-31T23:59:59Z"));
    assert(!cff::schedule_lineup::lineupMutationAllowed(open, "2026-09-01T00:00:00Z"));

    Json::Value locked = open;
    locked["status"] = "locked";
    locked["locked"] = true;
    assert(!cff::schedule_lineup::lineupMutationAllowed(locked, "2026-08-31T00:00:00Z"));
    assert(cff::schedule_lineup::lineupMutationAllowed(locked, "2026-08-31T00:00:00Z", true));
    assert(cff::schedule_lineup::canUnlockWeek(locked));

    Json::Value final = locked;
    final["status"] = "final";
    assert(!cff::schedule_lineup::lineupMutationAllowed(final, "", true));
    assert(!cff::schedule_lineup::canUnlockWeek(final));
    assert(cff::schedule_lineup::nextVersion(0) == 1);
    assert(cff::schedule_lineup::nextVersion(8) == 9);

    std::cout << "schedule lineup lifecycle tests passed\n";
    return 0;
}
