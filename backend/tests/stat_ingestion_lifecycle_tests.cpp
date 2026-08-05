#include "stat_ingestion_lifecycle.h"

#include <cassert>
#include <iostream>

int main() {
    using namespace cff::stat_ingestion_lifecycle;

    assert(canonicalToken(" Passing Yards ") == "passing_yards");
    assert(canonicalToken("Fumbles-Lost") == "fumbles_lost");

    Json::Value first(Json::objectValue);
    first["playerId"] = "player-1";
    first["season"] = 2026;
    first["week"] = 1;
    first["category"] = "Passing";
    first["statName"] = "Passing Yards";
    first["statValue"] = 250.0;
    first["gameId"] = Json::Int64(1001);
    first["team"] = "Test U";
    first["conference"] = "Test";

    auto same = first;
    same["category"] = "passing";
    same["statName"] = "passing-yards";
    assert(statRecordKey(first) == statRecordKey(same));
    assert(statSourceHash(first) == statSourceHash(same));

    auto correction = first;
    correction["statValue"] = 275.0;
    assert(statSourceHash(first) != statSourceHash(correction));

    assert(retryDelaySeconds(1) == 5);
    assert(retryDelaySeconds(2) == 10);
    assert(retryDelaySeconds(6) == 160);
    assert(retryDelaySeconds(10, 120) == 120);
    assert(retryDelaySeconds(10, 1200, 900) == 900);
    assert(retryableProviderFailure(429));
    assert(retryableProviderFailure(503));
    assert(retryableProviderFailure(0, true));
    assert(!retryableProviderFailure(400));
    assert(sourceFresh(300, 900));
    assert(!sourceFresh(901, 900));
    assert(recalculationStatus("scored") == "pending");
    assert(recalculationStatus("final") == "blocked_final");
    assert(recalculationStatus("unscored") == "not_required");
    assert(recalculationReason("final") == "final_week_immutable");

    std::cout << "stat ingestion lifecycle rules passed\n";
    return 0;
}
