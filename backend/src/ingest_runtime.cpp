#include "ingest_runtime.h"

#include <chrono>
#include <iostream>
#include <thread>
#include <utility>

namespace cff::ingest_runtime {

void logIngestResult(const std::string &label,
                     const cff::IngestResult &result,
                     std::ostream &output,
                     std::ostream &errors) {
    output << "[cfbd] " << label << " complete. inserted=" << result.ingested
           << " updated=" << result.updated
           << " api_calls=" << result.apiCalls << std::endl;
    for (const auto &error : result.errors) {
        errors << "[cfbd] " << label << " error: " << error << std::endl;
    }
}

bool runStartupIngest(bool enabled,
                      const IngestRunner &runner,
                      std::ostream &output,
                      std::ostream &errors) {
    if (!enabled) {
        return false;
    }

    output << "[cfbd] CFBD_INGEST_ON_STARTUP enabled; starting ingest..." << std::endl;
    logIngestResult("startup ingest", runner(), output, errors);
    return true;
}

void runScheduledIngestCycle(int intervalHours,
                             const IngestRunner &runner,
                             const SleepFunction &sleep,
                             std::ostream &output,
                             std::ostream &errors) {
    sleep(std::chrono::hours(intervalHours));
    output << "[cfbd] background ingest starting..." << std::endl;
    logIngestResult("background ingest", runner(), output, errors);
}

void configureCfbdIngest(bool ingestOnStartup,
                         const std::optional<int> &intervalHours,
                         IngestRunner runner) {
    runStartupIngest(ingestOnStartup, runner, std::cout, std::cerr);

    if (!intervalHours) {
        return;
    }

    const int configuredInterval = *intervalHours;
    std::thread([configuredInterval, runner = std::move(runner)]() mutable {
        std::cout << "[cfbd] background ingest enabled every "
                  << configuredInterval << " hour(s)." << std::endl;
        const SleepFunction sleep = [](std::chrono::hours duration) {
            std::this_thread::sleep_for(duration);
        };
        while (true) {
            runScheduledIngestCycle(
                configuredInterval,
                runner,
                sleep,
                std::cout,
                std::cerr
            );
        }
    }).detach();
}

} // namespace cff::ingest_runtime
