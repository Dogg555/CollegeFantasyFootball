#include "health_routes.h"

#include "app_config.h"
#include "email_delivery.h"
#include "http_security.h"

#include <algorithm>
#include <iostream>
#include <memory>
#include <utility>

#ifdef CFF_HAS_POSTGRES
#include <postgresql/libpq-fe.h>
#endif

namespace cff::health {
namespace {

bool emailDeliveryReady() {
    return cff::config::frontendBaseUrl().has_value()
        && cff::emailDeliveryConfigured();
}

#ifdef CFF_HAS_POSTGRES
struct PgConnDeleter {
    void operator()(PGconn *conn) const {
        if (conn) {
            PQfinish(conn);
        }
    }
};

using PgConnPtr = std::unique_ptr<PGconn, PgConnDeleter>;

bool databaseConfigured() {
    const auto url = cff::config::readEnv("DB_URL");
    return url && !url->empty();
}

PgConnPtr connectToDatabase() {
    const auto url = cff::config::readEnv("DB_URL");
    if (!url || url->empty()) {
        return nullptr;
    }

    auto conn = PgConnPtr{PQconnectdb(url->c_str())};
    if (PQstatus(conn.get()) != CONNECTION_OK) {
        std::cerr << "[auth] Failed to connect to Postgres: "
                  << PQerrorMessage(conn.get()) << std::endl;
        return nullptr;
    }
    return conn;
}
#endif

} // namespace

Json::Value buildHealthPayload(
    const std::optional<std::string> &jwtSecret,
    const std::unordered_set<std::string> &allowedOrigins) {
    Json::Value payload;
    const auto passwordMax = cff::config::maxPasswordLength();
    const auto passwordMin = std::min(
        cff::config::minPasswordLength(), passwordMax);

    payload["status"] = "ok";
    payload["service"] = "college-ff-api";
    payload["jwtSecretConfigured"] = jwtSecret.has_value();
    payload["allowedOriginsConfigured"] = !allowedOrigins.empty();
    payload["persistentDbRequired"] = cff::config::persistentDbRequired();
    payload["emailDeliveryConfigured"] = emailDeliveryReady();
    payload["emailVerificationRequired"] =
        cff::config::emailVerificationRequired();
    payload["passwordPolicy"]["minLength"] =
        static_cast<Json::UInt64>(passwordMin);
    payload["passwordPolicy"]["maxLength"] =
        static_cast<Json::UInt64>(passwordMax);

#ifdef CFF_HAS_POSTGRES
    payload["databaseConfigured"] = databaseConfigured();
    if (databaseConfigured()) {
        auto conn = connectToDatabase();
        payload["database"] = conn ? "ok" : "unavailable";
        if (!conn && cff::config::persistentDbRequired()) {
            payload["status"] = "degraded";
        }
    } else {
        payload["database"] = "not_configured";
        if (cff::config::persistentDbRequired()) {
            payload["status"] = "degraded";
        }
    }
#else
    payload["databaseConfigured"] = false;
    payload["database"] = "not_compiled";
    if (cff::config::persistentDbRequired()) {
        payload["status"] = "degraded";
    }
#endif

    return payload;
}

drogon::HttpStatusCode healthStatusCode(const Json::Value &payload) {
    const auto status = payload.isMember("status")
        ? payload["status"].asString()
        : "ok";
    if (cff::config::persistentDbRequired() && status != "ok") {
        return drogon::k503ServiceUnavailable;
    }
    return drogon::k200OK;
}

void registerHealthRoutes(
    drogon::HttpAppFramework &app,
    const std::optional<std::string> &jwtSecret,
    const std::unordered_set<std::string> &allowedOrigins) {
    const auto healthHandler = [jwtSecret, allowedOrigins](
        const drogon::HttpRequestPtr &,
        std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        const auto payload = buildHealthPayload(jwtSecret, allowedOrigins);
        auto response = drogon::HttpResponse::newHttpJsonResponse(payload);
        response->setStatusCode(healthStatusCode(payload));
        callback(response);
    };

    app.registerHandler("/health", healthHandler, {drogon::Get});
    app.registerHandler("/api/health", healthHandler, {drogon::Get});

    app.registerHandler(
        "/api/health",
        [allowedOrigins](
            const drogon::HttpRequestPtr &request,
            std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
            callback(cff::http::buildPreflightResponse(
                request, allowedOrigins));
        },
        {drogon::Options});
}

} // namespace cff::health
