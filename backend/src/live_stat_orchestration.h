#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cff::live_stats {

enum class RunStatus { queued, running, partial, succeeded, failed, duplicate, skipped };
enum class SourceState { fresh, stale, partial, unavailable, unknown };

struct SourceResult {
    std::string source;
    bool attempted{false};
    bool succeeded{false};
    std::size_t rows{0};
    std::string error;
    std::chrono::system_clock::time_point observedAt{};
};

struct RunDecision {
    bool start{false};
    std::string code;
};

struct RefreshDecision {
    bool enqueue{false};
    std::string code;
};

RunDecision mayStartRun(bool matchingRunActive,
                        bool matchingRunRecentlyCompleted,
                        bool force);

RunStatus aggregateStatus(const std::vector<SourceResult> &sources);

SourceState sourceState(const std::optional<std::chrono::system_clock::time_point> &lastSuccess,
                        bool latestRunPartial,
                        std::chrono::system_clock::time_point now,
                        std::chrono::minutes freshFor,
                        std::chrono::minutes staleAfter);

RefreshDecision shouldQueueScoringRefresh(bool statsChanged,
                                          bool weekFinalized,
                                          std::chrono::system_clock::time_point now,
                                          std::chrono::system_clock::time_point gameEndedAt,
                                          std::chrono::hours correctionWindow);

std::string toString(RunStatus status);
std::string toString(SourceState state);

} // namespace cff::live_stats
