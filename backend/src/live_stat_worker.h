#pragma once

#include <json/json.h>

#include <string>

namespace cff::live_stats {

struct WorkerRequest {
    int season{0};
    int week{0};
    bool force{false};
    std::string runKey;
};

int configuredLiveStatSeason();
int configuredLiveStatWeek();

// Claims a durable CFBD live-score refresh run, executes the existing cache
// adapter with bounded retries, and persists run/source/freshness telemetry.
Json::Value runCfbdLiveStatWorker(const WorkerRequest &request);

// Returns recent durable runs, source freshness, queue state, operator events,
// and the existing live-score cache health payload.
Json::Value liveStatOperatorStatus(int season = 0, int week = -1);

// Enables an optional detached worker using CFF_LIVE_STAT_ON_STARTUP and
// CFF_LIVE_STAT_INTERVAL_MINUTES. No worker starts unless one is configured.
void configureLiveStatWorker();

} // namespace cff::live_stats
