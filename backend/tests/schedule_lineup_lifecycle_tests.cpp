#include "schedule_lineup_lifecycle.h"

#include <cassert>
#include <iostream>
#include <set>
#include <string>

namespace {

Json::Value members(int count) {
    Json::Value result(Json::arrayValue);
    for (int index = count; index >= 1; --index) {
        Json::Value member(Json::objectValue);
        member["email"] = "Manager" + std::to_string(index) + "@Example.com";
        member["status"] = "Active";
        result.append(member);
    }
    return result;
}

void assertSchedule(int teamCount, int weeks) {
    const auto source = members(teamCount);
    const auto first = cff::schedule_lineup_lifecycle::buildDeterministicSchedule(
        source, "league-test", 2026, weeks);
    const auto second = cff::schedule_lineup_lifecycle::buildDeterministicSchedule(
        source, "league-test", 2026, weeks);
    assert(first == second);
    assert(first.size() == static_cast<Json::ArrayIndex>((teamCount / 2) * weeks));

    std::set<std::string> ids;
    std::set<std::string> weeklyManagers;
    int activeWeek = 1;
    for (const auto &matchup : first) {
        assert(ids.insert(matchup["id"].asString()).second);
        const int week = matchup["week"].asInt();
        if (week != activeWeek) {
            assert(weeklyManagers.size() == static_cast<std::size_t>(teamCount));
            weeklyManagers.clear();
            activeWeek = week;
        }
        assert(weeklyManagers.insert(matchup["homeManager"].asString()).second);
        if (!matchup["awayManager"].asString().empty()) {
            assert(weeklyManagers.insert(matchup["awayManager"].asString()).second);
        }
    }
    assert(weeklyManagers.size() == static_cast<std::size_t>(teamCount));
}

} // namespace

int main() {
    const auto order = cff::schedule_lineup_lifecycle::canonicalManagerOrder(members(4));
    assert(order.size() == 4);
    assert(order[0].asString() == "manager1@example.com");
    assert(order[3].asString() == "manager4@example.com");

    assertSchedule(4, 12);
    assertSchedule(6, 15);
    assertSchedule(8, 15);

    const auto hashA = cff::schedule_lineup_lifecycle::scheduleInputHash(order, 2026, 12);
    const auto hashB = cff::schedule_lineup_lifecycle::scheduleInputHash(order, 2026, 12);
    const auto hashC = cff::schedule_lineup_lifecycle::scheduleInputHash(order, 2026, 13);
    assert(hashA == hashB);
    assert(hashA != hashC);

    Json::Value expected(Json::objectValue);
    expected["expectedVersion"] = Json::Int64(7);
    assert(cff::schedule_lineup_lifecycle::expectedVersionMatches(7, expected));
    assert(!cff::schedule_lineup_lifecycle::expectedVersionMatches(8, expected));
    assert(!cff::schedule_lineup_lifecycle::expectedVersionMatches(0, Json::Value{Json::objectValue}));

    assert(cff::schedule_lineup_lifecycle::deadlinePassed(
        "2026-08-04T20:00:00Z", "2026-08-04T20:00:01Z"));
    assert(!cff::schedule_lineup_lifecycle::deadlinePassed(
        "2026-08-04T20:00:02Z", "2026-08-04T20:00:01Z"));
    assert(cff::schedule_lineup_lifecycle::lockedStatus("locked"));
    assert(cff::schedule_lineup_lifecycle::lockedStatus("Finalized"));
    assert(!cff::schedule_lineup_lifecycle::lockedStatus("open"));

    std::cout << "schedule lineup lifecycle tests passed\n";
    return 0;
}
