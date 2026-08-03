#include "ingest_runtime.h"

#include <chrono>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string &message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

cff::IngestResult sampleResult() {
    cff::IngestResult result;
    result.ingested = 12;
    result.updated = 4;
    result.apiCalls = 3;
    result.errors = {"first failure", "second failure"};
    return result;
}

void testResultLogging() {
    std::ostringstream output;
    std::ostringstream errors;

    cff::ingest_runtime::logIngestResult(
        "contract ingest",
        sampleResult(),
        output,
        errors
    );

    require(
        output.str() ==
            "[cfbd] contract ingest complete. inserted=12 updated=4 api_calls=3\n",
        "ingestion summary logging changed"
    );
    require(
        errors.str() ==
            "[cfbd] contract ingest error: first failure\n"
            "[cfbd] contract ingest error: second failure\n",
        "ingestion error logging changed"
    );
}

void testDisabledStartupDoesNothing() {
    std::ostringstream output;
    std::ostringstream errors;
    int calls = 0;

    const bool ran = cff::ingest_runtime::runStartupIngest(
        false,
        [&calls]() {
            ++calls;
            return sampleResult();
        },
        output,
        errors
    );

    require(!ran, "disabled startup ingest reported that it ran");
    require(calls == 0, "disabled startup ingest invoked the runner");
    require(output.str().empty(), "disabled startup ingest wrote standard output");
    require(errors.str().empty(), "disabled startup ingest wrote error output");
}

void testEnabledStartupRunsOnce() {
    std::ostringstream output;
    std::ostringstream errors;
    int calls = 0;

    const bool ran = cff::ingest_runtime::runStartupIngest(
        true,
        [&calls]() {
            ++calls;
            return sampleResult();
        },
        output,
        errors
    );

    require(ran, "enabled startup ingest did not report execution");
    require(calls == 1, "enabled startup ingest did not invoke the runner exactly once");
    require(
        output.str() ==
            "[cfbd] CFBD_INGEST_ON_STARTUP enabled; starting ingest...\n"
            "[cfbd] startup ingest complete. inserted=12 updated=4 api_calls=3\n",
        "startup ingest output changed"
    );
    require(
        errors.str() ==
            "[cfbd] startup ingest error: first failure\n"
            "[cfbd] startup ingest error: second failure\n",
        "startup ingest error output changed"
    );
}

void testScheduledCycleSleepsBeforeRunning() {
    std::ostringstream output;
    std::ostringstream errors;
    std::vector<std::string> events;

    cff::ingest_runtime::runScheduledIngestCycle(
        6,
        [&events]() {
            events.push_back("run");
            return sampleResult();
        },
        [&events](std::chrono::hours duration) {
            require(duration == std::chrono::hours(6), "scheduled interval changed");
            events.push_back("sleep");
        },
        output,
        errors
    );

    require(
        events == std::vector<std::string>({"sleep", "run"}),
        "scheduled ingest no longer waits before running"
    );
    require(
        output.str() ==
            "[cfbd] background ingest starting...\n"
            "[cfbd] background ingest complete. inserted=12 updated=4 api_calls=3\n",
        "background ingest output changed"
    );
    require(
        errors.str() ==
            "[cfbd] background ingest error: first failure\n"
            "[cfbd] background ingest error: second failure\n",
        "background ingest error output changed"
    );
}

} // namespace

int main() {
    try {
        testResultLogging();
        testDisabledStartupDoesNothing();
        testEnabledStartupRunsOnce();
        testScheduledCycleSleepsBeforeRunning();
        std::cout << "ingest runtime contracts passed" << std::endl;
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "ingest runtime contract failure: " << error.what() << std::endl;
        return 1;
    }
}
