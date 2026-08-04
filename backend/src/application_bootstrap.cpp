#include "application_bootstrap.h"

#include <iostream>

#ifdef DROGON_FOUND
#include <drogon/drogon.h>

#include "app_composition.h"
#include "app_config.h"
#include "cfbd_ingest.h"
#include "ingest_runtime.h"
#include "server_runtime.h"
#endif

namespace cff::application {

int runApplication() {
#ifdef DROGON_FOUND
    auto &app = drogon::app();

    // Load environment configuration once at startup. Policy helpers used by
    // request handlers remain centralized in app_config.cpp.
    const auto runtimeConfig = cff::config::loadRuntimeConfig();
    const auto &jwtSecret = runtimeConfig.jwtSecret;
    const auto &allowedOrigins = runtimeConfig.allowedOrigins;

    const bool useSsl = cff::server_runtime::configureSecurityAndCors(
        app,
        runtimeConfig
    );

    cff::ingest_runtime::configureCfbdIngest(
        runtimeConfig.ingestOnStartup,
        runtimeConfig.ingestIntervalHours,
        cff::runCfbdIngestOnce
    );

    cff::server_runtime::configureListener(
        app,
        runtimeConfig.port,
        useSsl
    );

    cff::app_composition::registerApplicationRoutes(
        app,
        jwtSecret,
        allowedOrigins
    );

    app.run();
#else
    // Stub output to avoid hard dependency on Drogon in early scaffolding.
    std::cout << "College Fantasy Football backend scaffold (Drogon not linked)." << std::endl;
    std::cout << "Build with -DDROGON_FOUND=ON and link Drogon to run the HTTP server." << std::endl;
#endif
    return 0;
}

} // namespace cff::application
