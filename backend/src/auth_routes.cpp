#include "auth_routes.h"

#include "app_config.h"
#include "auth_controller.h"
#include "auth_core.h"
#include "auth_session_store.h"
#include "email_delivery.h"

#include <algorithm>
#include <iostream>
#include <memory>
#include <optional>
#include <string>

#ifdef CFF_HAS_POSTGRES
#include <postgresql/libpq-fe.h>
#endif

namespace cff::auth {
namespace {

using cff::config::emailVerificationRequired;
using cff::config::frontendBaseUrl;
using cff::config::maxPasswordLength;
using cff::config::minPasswordLength;
using cff::config::persistentDbRequired;
using cff::config::readEnv;
using cff::config::sharedSecretAuthAllowed;

bool emailDeliveryReady() {
    return frontendBaseUrl().has_value() && cff::emailDeliveryConfigured();
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
    const auto url = readEnv("DB_URL");
    return url && !url->empty();
}

PgConnPtr connectToDatabase() {
    const auto url = readEnv("DB_URL");
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

Json::Value readinessPayload() {
    Json::Value payload;
    const auto passwordMax = maxPasswordLength();
    const auto passwordMin = std::min(minPasswordLength(), passwordMax);
    payload["status"] = "ok";
    payload["service"] = "college-ff-api";
    payload["persistentDbRequired"] = persistentDbRequired();
    payload["emailVerificationRequired"] = emailVerificationRequired();
    payload["emailDeliveryConfigured"] = emailDeliveryReady();
    payload["emailProvider"] = cff::emailDeliveryProvider();
    payload["frontendBaseUrlConfigured"] = frontendBaseUrl().has_value();
    payload["passwordPolicy"]["minLength"] = static_cast<Json::UInt64>(passwordMin);
    payload["passwordPolicy"]["maxLength"] = static_cast<Json::UInt64>(passwordMax);
#ifdef CFF_HAS_POSTGRES
    payload["databaseConfigured"] = databaseConfigured();
    if (databaseConfigured()) {
        auto conn = connectToDatabase();
        payload["database"] = conn ? "ok" : "unavailable";
        if (!conn) {
            payload["status"] = "degraded";
        }
    } else {
        payload["database"] = "not_configured";
        if (persistentDbRequired()) {
            payload["status"] = "degraded";
        }
    }
#else
    payload["databaseConfigured"] = false;
    payload["database"] = "not_compiled";
    if (persistentDbRequired()) {
        payload["status"] = "degraded";
    }
#endif
    const bool dbReady = !persistentDbRequired()
        || (payload.isMember("database") && payload["database"].asString() == "ok");
    const bool emailReady = !emailVerificationRequired() || emailDeliveryReady();
    payload["signupEnabled"] = dbReady;
    payload["loginEnabled"] = dbReady;
    payload["emailFlowsEnabled"] = emailDeliveryReady();
    payload["ready"] = dbReady && emailReady;
    if (!dbReady) {
        payload["message"] = "Authentication database is not ready.";
    } else if (!emailReady) {
        payload["status"] = "degraded";
        payload["message"] = "Email verification is required, but transactional email is not configured.";
    } else {
        payload["message"] = "Authentication is ready.";
    }
    return payload;
}

bool storageUnavailable() {
#ifdef CFF_HAS_POSTGRES
    if (!persistentDbRequired()) {
        return false;
    }
    if (!databaseConfigured()) {
        return true;
    }
    return !connectToDatabase();
#else
    return persistentDbRequired();
#endif
}

std::optional<std::string> bearerToken(const drogon::HttpRequestPtr &req) {
    return bearerTokenFromHeader(req->getHeader("authorization"));
}

bool isAuthorized(const drogon::HttpRequestPtr &req,
                  const std::optional<std::string> &secret) {
    const auto token = bearerToken(req);
    if (!token) {
        return false;
    }
#ifdef CFF_HAS_POSTGRES
    if (persistentDbRequired() && !databaseConfigured()) {
        return false;
    }
#else
    if (persistentDbRequired()) {
        return false;
    }
#endif
    if (sharedSecretAuthAllowed() && secret && *token == *secret) {
        return true;
    }
    return emailForSessionToken(*token).has_value();
}

} // namespace

void registerAuthRoutes(drogon::HttpAppFramework &app,
                        const std::optional<std::string> &jwtSecret) {
    app.registerHandler("/api/auth/validate",
                        [jwtSecret](const drogon::HttpRequestPtr &req,
                                    std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
                            auto resp = drogon::HttpResponse::newHttpResponse();
                            if (storageUnavailable()) {
                                Json::Value error;
                                error["valid"] = false;
                                error["unavailable"] = true;
                                error["error"] = "Authentication service is temporarily unavailable";
                                resp->setStatusCode(drogon::k503ServiceUnavailable);
                                resp->setBody(error.toStyledString());
                                resp->addHeader("Content-Type", "application/json");
                                callback(resp);
                                return;
                            }

                            const auto token = bearerToken(req);
                            const bool authorized = isAuthorized(req, jwtSecret);
                            Json::Value payload;
                            payload["valid"] = authorized;
                            if (authorized && token) {
                                if (const auto email = emailForSessionToken(*token)) {
                                    payload["email"] = *email;
                                }
                            }
                            resp->setStatusCode(authorized ? drogon::k200OK : drogon::k401Unauthorized);
                            resp->setBody(payload.toStyledString());
                            resp->addHeader("Content-Type", "application/json");
                            callback(resp);
                        },
                        {drogon::Get});

    app.registerHandler("/api/auth/status",
                        [](const drogon::HttpRequestPtr &,
                           std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
                            auto resp = drogon::HttpResponse::newHttpJsonResponse(readinessPayload());
                            resp->setStatusCode(drogon::k200OK);
                            callback(resp);
                        },
                        {drogon::Get});

    app.registerHandler("/api/auth/login",
                        [jwtSecret](const drogon::HttpRequestPtr &req,
                                    std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
                            handleLogin(req, std::move(callback), jwtSecret);
                        },
                        {drogon::Post});

    app.registerHandler("/api/auth/signup",
                        [jwtSecret](const drogon::HttpRequestPtr &req,
                                    std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
                            handleSignup(req, std::move(callback), jwtSecret);
                        },
                        {drogon::Post});

    app.registerHandler("/api/auth/logout",
                        [](const drogon::HttpRequestPtr &req,
                           std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
                            handleLogout(req, std::move(callback));
                        },
                        {drogon::Post});

    app.registerHandler("/api/auth/verify-email",
                        [](const drogon::HttpRequestPtr &req,
                           std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
                            handleVerifyEmail(req, std::move(callback));
                        },
                        {drogon::Post});

    app.registerHandler("/api/auth/resend-verification",
                        [](const drogon::HttpRequestPtr &req,
                           std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
                            handleResendVerification(req, std::move(callback));
                        },
                        {drogon::Post});

    app.registerHandler("/api/auth/request-password-reset",
                        [](const drogon::HttpRequestPtr &req,
                           std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
                            handleRequestPasswordReset(req, std::move(callback));
                        },
                        {drogon::Post});

    app.registerHandler("/api/auth/reset-password",
                        [](const drogon::HttpRequestPtr &req,
                           std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
                            handleResetPassword(req, std::move(callback));
                        },
                        {drogon::Post});
}

} // namespace cff::auth
