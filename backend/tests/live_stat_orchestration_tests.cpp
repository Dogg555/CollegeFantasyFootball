#include "live_stat_orchestration.h"

#include <cassert>
#include <chrono>
#include <vector>

int main() {
    using namespace cff::live_stats;
    using clock = std::chrono::system_clock;
    const auto now = clock::now();

    assert(!mayStartRun(true, false, false).start);
    assert(mayStartRun(true, false, false).code == "ingest_already_running");
    assert(!mayStartRun(false, true, false).start);
    assert(mayStartRun(false, true, true).start);

    std::vector<SourceResult> allGood{{"games", true, true, 10, "", now}, {"stats", true, true, 20, "", now}};
    assert(aggregateStatus(allGood) == RunStatus::succeeded);
    allGood[1].succeeded = false;
    assert(aggregateStatus(allGood) == RunStatus::partial);
    allGood[0].succeeded = false;
    assert(aggregateStatus(allGood) == RunStatus::failed);

    assert(sourceState(now - std::chrono::minutes(3), false, now,
                       std::chrono::minutes(5), std::chrono::minutes(30)) == SourceState::fresh);
    assert(sourceState(now - std::chrono::minutes(10), false, now,
                       std::chrono::minutes(5), std::chrono::minutes(30)) == SourceState::stale);
    assert(sourceState(now - std::chrono::minutes(40), false, now,
                       std::chrono::minutes(5), std::chrono::minutes(30)) == SourceState::unavailable);
    assert(sourceState(std::nullopt, true, now,
                       std::chrono::minutes(5), std::chrono::minutes(30)) == SourceState::partial);

    assert(shouldQueueScoringRefresh(true, false, now, now - std::chrono::hours(2), std::chrono::hours(24)).enqueue);
    assert(!shouldQueueScoringRefresh(false, false, now, now, std::chrono::hours(24)).enqueue);
    assert(!shouldQueueScoringRefresh(true, true, now, now, std::chrono::hours(24)).enqueue);
    assert(!shouldQueueScoringRefresh(true, false, now, now - std::chrono::hours(30), std::chrono::hours(24)).enqueue);

    return 0;
}
