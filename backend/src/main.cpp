#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>

#ifdef DROGON_FOUND
#include <drogon/drogon.h>
#include "league_models.h"
#include "handlers/league_handler.h"
#endif

#ifdef DROGON_FOUND
namespace {
std::optional<std::string> readEnv(const std::string &key) {
    const char *val = std::getenv(key.c_str());
    if (val == nullptr) {
        return std::nullopt;
    }
    return std::string{val};
}

bool isAuthorized(const drogon::HttpRequestPtr &req, const std::string &secret) {
    const auto authHeader = req->getHeader("authorization");
    if (authHeader.size() < 8) {
        return false;
    }
    constexpr std::string_view bearerPrefix = "Bearer ";
    if (authHeader.rfind(bearerPrefix, 0) != 0) {
        return false;
    }
    auto token = authHeader.substr(bearerPrefix.size());
    return token == secret;
}
} // namespace
#endif

int main(int argc, char* argv[]) {
#ifdef DROGON_FOUND
    auto &app = drogon::app();

    // Environment configuration
    const auto port = readEnv("PORT").value_or("8080");
    const auto jwtSecret = readEnv("JWT_SECRET");
    const auto sslCert = readEnv("SSL_CERT_FILE");
    const auto sslKey = readEnv("SSL_KEY_FILE");
    const auto allowedOriginEnv = readEnv("ALLOWED_ORIGINS");

    if (!jwtSecret.has_value()) {
        std::cerr << "[security] JWT_SECRET is not set; secure endpoints will reject all requests." << std::endl;
    }

    // SSL enablement when certs are available
    if (sslCert && sslKey) {
        app.setSSLFiles(sslCert.value(), sslKey.value());
        std::cout << "[security] SSL enabled with provided certificate and key." << std::endl;
    } else {
        std::cout << "[security] SSL not configured. For testing only. Provide SSL_CERT_FILE and SSL_KEY_FILE to enable HTTPS." << std::endl;
    }

    // Minimal CORS handling via post-routing advice
    std::unordered_set<std::string> allowedOrigins;
    if (allowedOriginEnv) {
        // Comma-separated list of origins
        std::string list = allowedOriginEnv.value();
        std::size_t start = 0;
        while (true) {
            auto pos = list.find(',', start);
            auto origin = list.substr(start, pos == std::string::npos ? std::string::npos : pos - start);
            if (!origin.empty()) {
                allowedOrigins.insert(origin);
            }
            if (pos == std::string::npos) {
                break;
            }
            start = pos + 1;
        }
    }

    app.registerPostHandlingAdvice([allowedOrigins](const drogon::HttpRequestPtr &req,
                                                    const drogon::HttpResponsePtr &resp) {
        if (!allowedOrigins.empty()) {
            auto origin = req->getHeader("Origin");
            if (allowedOrigins.find(origin) != allowedOrigins.end()) {
                resp->addHeader("Access-Control-Allow-Origin", origin);
            }
        } else {
            resp->addHeader("Access-Control-Allow-Origin", "*");
        }
        resp->addHeader("Access-Control-Allow-Headers", "Authorization, Content-Type");
        resp->addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    });

    app.addListener("0.0.0.0", static_cast<unsigned short>(std::stoi(port)))
        .setThreadNum(std::thread::hardware_concurrency())
        .registerHandler("/health", [](const drogon::HttpRequestPtr&, std::function<void (const drogon::HttpResponsePtr &)> &&callback) {
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k200OK);
            resp->setBody("ok");
            callback(resp);
        })
        .registerHandler("/api/secure/ping",
                         [jwtSecret](const drogon::HttpRequestPtr& req, std::function<void (const drogon::HttpResponsePtr &)> &&callback) {
                             auto resp = drogon::HttpResponse::newHttpResponse();
                             if (!jwtSecret || !isAuthorized(req, jwtSecret.value())) {
                                 resp->setStatusCode(drogon::k401Unauthorized);
                                 resp->setBody("unauthorized");
                                 callback(resp);
                                 return;
                             }
                             resp->setStatusCode(drogon::k200OK);
                             resp->setBody(R"({"status":"ok","scope":"secure"})");
                             resp->addHeader("Content-Type", "application/json");
                             callback(resp);
                        },
                         {drogon::Post, drogon::Get})
        .registerHandler("/api/auth/validate",
                         [jwtSecret](const drogon::HttpRequestPtr& req, std::function<void (const drogon::HttpResponsePtr &)> &&callback) {
                             auto resp = drogon::HttpResponse::newHttpResponse();
                             const bool authorized = jwtSecret && isAuthorized(req, jwtSecret.value());
                             resp->setStatusCode(authorized ? drogon::k200OK : drogon::k401Unauthorized);
                             resp->setBody(authorized ? R"({"valid":true})" : R"({"valid":false})");
                             resp->addHeader("Content-Type", "application/json");
                             callback(resp);
                         },
                         {drogon::Get})
        .registerHandler("/api/leagues",
                         &cff::handlers::handleCreateLeague,
                         {drogon::Post})
        .run();
#else
    // Stub output to avoid hard dependency on Drogon in early scaffolding.
    std::cout << "College Fantasy Football backend scaffold (Drogon not linked)." << std::endl;
    std::cout << "Build with -DDROGON_FOUND=ON and link Drogon to run the HTTP server." << std::endl;
#endif
    return 0;
}
