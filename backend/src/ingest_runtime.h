#pragma once

#include "cfbd_ingest.h"

#include <chrono>
#include <functional>
#include <optional>
#include <ostream>

namespace cff::ingest_runtime {

using IngestRunner = std::function<cff::IngestResult()>;
using SleepFunction = std::function<void(std::chrono::hours)>;

void logIngestResult(const std::string &label,
                     const cff::IngestResult &result,
                     std::ostream &output,
                     std::ostream &errors);

bool runStartupIngest(bool enabled,
                      const IngestRunner &runner,
                      std::ostream &output,
                      std::ostream &errors);

void runScheduledIngestCycle(int intervalHours,
                             const IngestRunner &runner,
                             const SleepFunction &sleep,
                             std::ostream &output,
                             std::ostream &errors);

void configureCfbdIngest(bool ingestOnStartup,
                         const std::optional<int> &intervalHours,
                         IngestRunner runner);

} // namespace cff::ingest_runtime
