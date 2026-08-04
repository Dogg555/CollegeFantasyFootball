#include "league_schedule.h"

#include <algorithm>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

void expect(bool condition, const std::string &message) {
    if (!condition) {
        std::cerr << "league_schedule_tests failed: " << message << std::endl;
        std::exit(1);
    }
}

Json::Value member(const std::string &email, const std::string &status = "Active") {
    Json::Value value;
    value["email"] = email;
    value["status"] = status;
    return value;
}

Json::Value members(std::initializer_list<Json::Value> values) {
    Json::Value result(Json::arrayValue);
    for (const auto &value : values) {
        result.append(value);
    }
    return result;
}

std::pair<std::string, std::string> normalizedPair(const Json::Value &matchup) {
    auto first = matchup["homeManager"].asString();
    auto second = matchup["awayManager"].asString();
    if (second < first) {
        std::swap(first, second);
    }
    return {first, second};
}

void testActiveMemberCompatibility() {
    const auto result = cff::league_schedule::activeMembers(members({
        member("active@example.com"),
        member("invited@example.com", "Invited"),
        member("removed-upper@example.com", "Removed"),
        member("removed-lower@example.com", "removed")
    }));

    expect(result.size() == 2, "removed members must stay excluded");
    expect(result[0]["email"].asString() == "active@example.com", "active member order changed");
    expect(result[1]["email"].asString() == "invited@example.com", "legacy non-removed membership behavior changed");
}

void testFourTeamRoundRobin() {
    const auto leagueMembers = members({
        member("a@example.com"),
        member("b@example.com"),
        member("c@example.com"),
        member("d@example.com")
    });

    const auto schedule = cff::league_schedule::buildSeasonSchedule(
        leagueMembers,
        "league-four",
        3,
        [](const std::string &email) {
            return email == "a@example.com" ? 10.0 : 5.0;
        }
    );

    expect(schedule.size() == 6, "four teams must produce two games for each of three rounds");
    std::set<std::pair<std::string, std::string>> pairs;
    std::map<int, int> gamesByWeek;
    for (const auto &matchup : schedule) {
        pairs.insert(normalizedPair(matchup));
        ++gamesByWeek[matchup["week"].asInt()];
        expect(matchup["leagueId"].asString() == "league-four", "league id contract changed");
        expect(matchup["status"].asString() == "scheduled", "new matchup status changed");
        expect(matchup["createdAt"].asString().empty(), "createdAt compatibility value changed");
    }
    expect(pairs.size() == 6, "four-team round robin must contain every unique pairing once");
    expect(gamesByWeek[1] == 2 && gamesByWeek[2] == 2 && gamesByWeek[3] == 2,
           "four-team weekly game counts changed");
}

void testSixTeamRoundRobin() {
    Json::Value leagueMembers(Json::arrayValue);
    for (int index = 1; index <= 6; ++index) {
        leagueMembers.append(member("manager" + std::to_string(index) + "@example.com"));
    }

    const auto schedule = cff::league_schedule::buildSeasonSchedule(
        leagueMembers,
        "league-six",
        5,
        [](const std::string &) { return 0.0; }
    );

    expect(schedule.size() == 15, "six teams must produce fifteen games across five rounds");
    std::set<std::pair<std::string, std::string>> pairs;
    std::map<int, int> gamesByWeek;
    for (const auto &matchup : schedule) {
        pairs.insert(normalizedPair(matchup));
        ++gamesByWeek[matchup["week"].asInt()];
    }
    expect(pairs.size() == 15, "six-team round robin must contain every unique pairing once");
    for (int week = 1; week <= 5; ++week) {
        expect(gamesByWeek[week] == 3, "six-team week must contain three games");
    }
}

void testOddTeamByes() {
    Json::Value leagueMembers(Json::arrayValue);
    for (int index = 1; index <= 5; ++index) {
        leagueMembers.append(member("odd" + std::to_string(index) + "@example.com"));
    }

    const auto schedule = cff::league_schedule::buildSeasonSchedule(
        leagueMembers,
        "league-odd",
        5,
        [](const std::string &) { return 1.0; }
    );

    expect(schedule.size() == 15, "five teams must retain three schedule entries per week, including one bye");
    std::map<std::string, int> appearances;
    int byeEntries = 0;
    int playedGames = 0;
    for (const auto &matchup : schedule) {
        const auto home = matchup["homeManager"].asString();
        const auto away = matchup["awayManager"].asString();
        expect(!home.empty(), "home manager must not be empty after bye normalization");
        if (away.empty()) {
            ++byeEntries;
            expect(matchup["awayScore"].asDouble() == 0.0, "bye opponent score must remain zero");
            continue;
        }
        ++playedGames;
        ++appearances[home];
        ++appearances[away];
    }
    expect(byeEntries == 5, "five teams must produce one bye entry per week");
    expect(playedGames == 10, "five teams must produce two played games per week");
    for (int index = 1; index <= 5; ++index) {
        const auto email = "odd" + std::to_string(index) + "@example.com";
        expect(appearances[email] == 4, "each five-team manager must receive exactly one bye");
    }
}

void testSnakeDraftTurns() {
    Json::Value order(Json::arrayValue);
    order.append("a@example.com");
    order.append("b@example.com");
    order.append("c@example.com");
    order.append("d@example.com");

    const std::vector<std::string> expectedSnake{
        "a@example.com", "b@example.com", "c@example.com", "d@example.com",
        "d@example.com", "c@example.com", "b@example.com", "a@example.com",
        "a@example.com"
    };
    for (std::size_t index = 0; index < expectedSnake.size(); ++index) {
        expect(
            cff::league_schedule::currentDraftManager(order, static_cast<int>(index + 1), "SnAkE")
                == expectedSnake[index],
            "snake draft turn changed at pick " + std::to_string(index + 1)
        );
    }

    expect(cff::league_schedule::currentDraftManager(order, 5, "linear") == "a@example.com",
           "linear draft must restart at the first manager");
    expect(cff::league_schedule::currentDraftManager(Json::Value{Json::arrayValue}, 1).empty(),
           "empty draft order must return no manager");
}

} // namespace

int main() {
    testActiveMemberCompatibility();
    testFourTeamRoundRobin();
    testSixTeamRoundRobin();
    testOddTeamByes();
    testSnakeDraftTurns();
    std::cout << "league schedule contracts passed" << std::endl;
    return 0;
}
