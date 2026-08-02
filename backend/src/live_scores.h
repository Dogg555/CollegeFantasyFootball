#pragma once

#include <cstddef>
#include <json/json.h>
#include <string>
#include <vector>

namespace cff {

struct LiveScoreIngestResult {
    std::size_t apiCalls{0};
    std::size_t games{0};
    std::size_t liveGames{0};
    std::size_t scheduleGames{0};
    bool scheduleRefreshed{false};
    std::vector<std::string> errors;
};

// Refreshes the two-minute scoreboard overlay and periodically refreshes the
// full season schedule. The public payload is a merged, week-aware cache.
LiveScoreIngestResult runLiveScoreIngestOnce();
Json::Value cachedLiveScorePayload();
Json::Value liveScoreIngestStatus();

} // namespace cff
