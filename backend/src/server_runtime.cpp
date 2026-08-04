#include "server_runtime.h"

#include "http_security.h"

#include <iostream>
#include <thread>

namespace cff::server_runtime {

bool configureSecurityAndCors(
    drogon::HttpAppFramework &app,
    const cff::config::RuntimeConfig &runtimeConfig) {
    if (!runtimeConfig.jwtSecret.has_value()) {
        std::cerr << "[security] JWT_SECRET is not set; secure endpoints will reject all requests."
                  << std::endl;
    }

    const bool useSsl = runtimeConfig.sslEnabled();
    if (runtimeConfig.sslCert && runtimeConfig.sslKey) {
        app.setSSLFiles(runtimeConfig.sslCert.value(), runtimeConfig.sslKey.value());
        std::cout << "[security] SSL enabled with provided certificate and key." << std::endl;
    } else {
        std::cout
            << "[security] SSL not configured. For testing only. Provide SSL_CERT_FILE and "
               "SSL_KEY_FILE to enable HTTPS."
            << std::endl;
    }

    if (!runtimeConfig.allowedOriginsConfigured) {
        std::cout << "[security] ALLOWED_ORIGINS not set; cross-origin requests will be blocked."
                  << std::endl;
    }

    // Authoritative lifecycle modules return some responses from sync advice,
    // before a route handler runs. Pre-sending advice is the one response
    // boundary shared by normal handlers and those early lifecycle responses.
    const auto allowedOrigins = runtimeConfig.allowedOrigins;
    app.registerPreSendingAdvice(
        [allowedOrigins](const drogon::HttpRequestPtr &request,
                         const drogon::HttpResponsePtr &response) {
            cff::http::applyCorsHeaders(request, response, allowedOrigins);
        });

    return useSsl;
}

void configureListener(
    drogon::HttpAppFramework &app,
    const std::string &port,
    bool useSsl) {
    app.addListener("0.0.0.0", static_cast<unsigned short>(std::stoi(port)), useSsl)
        .setThreadNum(std::thread::hardware_concurrency());
}

} // namespace cff::server_runtime
