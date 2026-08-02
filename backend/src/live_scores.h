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
    std::vector<std::string> errors;
};

LiveScoreIngestResult runLiveScoreIngestOnce();
Json::Value cachedLiveScorePayload();
Json::Value liveScoreIngestStatus();

} // namespace cff
