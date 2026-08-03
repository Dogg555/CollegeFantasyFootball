#include <iostream>
#include <string>
#include <thread>

#ifdef DROGON_FOUND
#include <drogon/drogon.h>

#include "app_config.h"
#include "auth_routes.h"
#include "cfbd_ingest.h"
#include "health_routes.h"
#include "http_security.h"
#include "ingest_runtime.h"
#include "league_routes.h"
#include "operations_routes.h"
#include "public_routes.h"
#endif

int main(int argc, char* argv[]) {
#ifdef DROGON_FOUND
    auto &app = drogon::app();

    // Load environment configuration once at startup. Policy helpers used by
    // request handlers remain centralized in app_config.cpp.
    const auto runtimeConfig = cff::config::loadRuntimeConfig();
    const auto &port = runtimeConfig.port;
    const auto &jwtSecret = runtimeConfig.jwtSecret;
    const auto &sslCert = runtimeConfig.sslCert;
    const auto &sslKey = runtimeConfig.sslKey;

    if (!jwtSecret.has_value()) {
        std::cerr << "[security] JWT_SECRET is not set; secure endpoints will reject all requests." << std::endl;
    }

    // SSL enablement when certs are available
    const bool useSsl = runtimeConfig.sslEnabled();
    if (sslCert && sslKey) {
        app.setSSLFiles(sslCert.value(), sslKey.value());
        std::cout << "[security] SSL enabled with provided certificate and key." << std::endl;
    } else {
        std::cout << "[security] SSL not configured. For testing only. Provide SSL_CERT_FILE and SSL_KEY_FILE to enable HTTPS." << std::endl;
    }

    // Minimal CORS handling via post-routing advice
    const auto &allowedOrigins = runtimeConfig.allowedOrigins;
    if (!runtimeConfig.allowedOriginsConfigured) {
        std::cout << "[security] ALLOWED_ORIGINS not set; cross-origin requests will be blocked." << std::endl;
    }

    app.registerPostHandlingAdvice([allowedOrigins](const drogon::HttpRequestPtr &req,
                                                    const drogon::HttpResponsePtr &resp) {
        cff::http::applyCorsHeaders(req, resp, allowedOrigins);
    });

    cff::ingest_runtime::configureCfbdIngest(
        runtimeConfig.ingestOnStartup,
        runtimeConfig.ingestIntervalHours,
        cff::runCfbdIngestOnce
    );

    app.addListener("0.0.0.0", static_cast<unsigned short>(std::stoi(port)), useSsl)
        .setThreadNum(std::thread::hardware_concurrency());

    cff::health::registerHealthRoutes(app, jwtSecret, allowedOrigins);

    cff::auth::registerAuthRoutes(app, jwtSecret, allowedOrigins);

    cff::operations::registerOperationsRoutes(app, jwtSecret, allowedOrigins);

    cff::league::registerLeagueRoutes(app, jwtSecret, allowedOrigins);

    cff::public_api::registerPublicRoutes(app, allowedOrigins);

    app.run();
#else
    // Stub output to avoid hard dependency on Drogon in early scaffolding.
    std::cout << "College Fantasy Football backend scaffold (Drogon not linked)." << std::endl;
    std::cout << "Build with -DDROGON_FOUND=ON and link Drogon to run the HTTP server." << std::endl;
#endif
    return 0;
}
