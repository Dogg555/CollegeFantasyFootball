#include "scoring_lifecycle.h"

#include <cassert>
#include <cmath>
#include <iostream>

namespace {

bool closeTo(double left, double right) {
    return std::fabs(left - right) < 0.000001;
}

Json::Value member(const std::string &email, const std::string &team) {
    Json::Value value(Json::objectValue);
    value["email"] = email;
    value["teamName"] = team;
    value["status"] = "Active";
    value["role"] = "member";
    return value;
}

Json::Value matchup(const std::string &home,
                    const std::string &away,
                    double homeScore,
                    double awayScore,
                    const std::string &status = "final",
                    int season = 2026) {
    Json::Value value(Json::objectValue);
    value["homeManager"] = home;
    value["awayManager"] = away;
    value["homeScore"] = homeScore;
    value["awayScore"] = awayScore;
    value["status"] = status;
    value["season"] = season;
    return value;
}

} // namespace

int main() {
    Json::Value settings(Json::objectValue);
    settings["passingYardsPerPoint"] = 25;
    settings["passingTd"] = 4;
    settings["interception"] = -2;
    settings["rushingYardsPerPoint"] = 10;
    settings["rushingTd"] = 6;
    settings["receivingYardsPerPoint"] = 10;
    settings["receivingTd"] = 6;
    settings["reception"] = 1;
    settings["fumbleLost"] = -2;
    settings["twoPointConversion"] = 2;

    assert(closeTo(cff::scoring_lifecycle::fantasyPointsForStat(settings, "passing", "passing yards", 250), 10));
    assert(closeTo(cff::scoring_lifecycle::fantasyPointsForStat(settings, "passing", "pass TD", 3), 12));
    assert(closeTo(cff::scoring_lifecycle::fantasyPointsForStat(settings, "receiving", "receptions", 8), 8));
    assert(closeTo(cff::scoring_lifecycle::fantasyPointsForStat(settings, "misc", "fumbles lost", 1), -2));

    Json::Value body(Json::objectValue);
    body["expectedVersion"] = Json::Int64(4);
    assert(cff::scoring_lifecycle::expectedVersionMatches(4, body, true));
    assert(!cff::scoring_lifecycle::expectedVersionMatches(5, body, true));
    assert(!cff::scoring_lifecycle::expectedVersionMatches(0, Json::Value{Json::objectValue}, true));
    assert(cff::scoring_lifecycle::expectedVersionMatches(0, Json::Value{Json::objectValue}, false));
    assert(cff::scoring_lifecycle::finalizedStatus("Final"));
    assert(cff::scoring_lifecycle::scoredStatus("scored"));

    Json::Value members(Json::arrayValue);
    members.append(member("alpha@example.test", "Alpha"));
    members.append(member("bravo@example.test", "Bravo"));
    members.append(member("charlie@example.test", "Charlie"));
    members.append(member("delta@example.test", "Delta"));

    Json::Value matchups(Json::arrayValue);
    matchups.append(matchup("alpha@example.test", "bravo@example.test", 100, 90));
    matchups.append(matchup("charlie@example.test", "delta@example.test", 80, 80));
    matchups.append(matchup("alpha@example.test", "charlie@example.test", 70, 95));
    matchups.append(matchup("bravo@example.test", "delta@example.test", 200, 1, "scheduled"));
    matchups.append(matchup("bravo@example.test", "delta@example.test", 110, 100, "final", 2025));

    const auto standings = cff::scoring_lifecycle::standingsFromFinalMatchups(members, matchups, 2026);
    assert(standings.size() == 4);
    assert(standings[0]["email"].asString() == "charlie@example.test");
    assert(standings[0]["wins"].asInt() == 1);
    assert(standings[0]["ties"].asInt() == 1);
    assert(standings[0]["gamesPlayed"].asInt() == 2);
    assert(standings[1]["email"].asString() == "alpha@example.test");
    assert(standings[1]["wins"].asInt() == 1);
    assert(standings[1]["losses"].asInt() == 1);
    assert(standings[2]["ties"].asInt() == 1);
    assert(standings[3]["gamesPlayed"].asInt() == 1);

    std::cout << "scoring lifecycle rules passed\n";
    return 0;
}
