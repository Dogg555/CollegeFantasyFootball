#include "live_stat_orchestration.h"

namespace cff::live_stats {

RunDecision mayStartRun(bool matchingRunActive,
                        bool matchingRunRecentlyCompleted,
                        bool force) {
    if (matchingRunActive) return {false, "ingest_already_running"};
    if (matchingRunRecentlyCompleted && !force) return {false, "duplicate_ingest"};
    return {true, force ? "forced" : "accepted"};
}

RunStatus aggregateStatus(const std::vector<SourceResult> &sources) {
    if (sources.empty()) return RunStatus::failed;
    std::size_t succeeded = 0;
    std::size_t attempted = 0;
    for (const auto &source : sources) {
        if (!source.attempted) continue;
        ++attempted;
        if (source.succeeded) ++succeeded;
    }
    if (attempted == 0) return RunStatus::skipped;
    if (succeeded == attempted) return RunStatus::succeeded;
    if (succeeded > 0) return RunStatus::partial;
    return RunStatus::failed;
}

SourceState sourceState(const std::optional<std::chrono::system_clock::time_point> &lastSuccess,
                        bool latestRunPartial,
                        std::chrono::system_clock::time_point now,
                        std::chrono::minutes freshFor,
                        std::chrono::minutes staleAfter) {
    if (!lastSuccess) return latestRunPartial ? SourceState::partial : SourceState::unknown;
    const auto age = std::chrono::duration_cast<std::chrono::minutes>(now - *lastSuccess);
    if (age <= freshFor) return latestRunPartial ? SourceState::partial : SourceState::fresh;
    if (age <= staleAfter) return latestRunPartial ? SourceState::partial : SourceState::stale;
    return SourceState::unavailable;
}

RefreshDecision shouldQueueScoringRefresh(bool statsChanged,
                                          bool weekFinalized,
                                          std::chrono::system_clock::time_point now,
                                          std::chrono::system_clock::time_point gameEndedAt,
                                          std::chrono::hours correctionWindow) {
    if (!statsChanged) return {false, "no_stat_changes"};
    if (weekFinalized) return {false, "week_finalized"};
    if (now > gameEndedAt + correctionWindow) return {false, "correction_window_closed"};
    return {true, "stats_changed"};
}

std::string toString(RunStatus status) {
    switch (status) {
        case RunStatus::queued: return "queued";
        case RunStatus::running: return "running";
        case RunStatus::partial: return "partial";
        case RunStatus::succeeded: return "succeeded";
        case RunStatus::failed: return "failed";
        case RunStatus::duplicate: return "duplicate";
        case RunStatus::skipped: return "skipped";
    }
    return "failed";
}

std::string toString(SourceState state) {
    switch (state) {
        case SourceState::fresh: return "fresh";
        case SourceState::stale: return "stale";
        case SourceState::partial: return "partial";
        case SourceState::unavailable: return "unavailable";
        case SourceState::unknown: return "unknown";
    }
    return "unknown";
}

} // namespace cff::live_stats
