#include "http_security.h"

#include "app_config.h"
#include "auth_core.h"
#include "auth_session_store.h"

namespace cff::http {
namespace {

bool databaseConfigured() {
    const auto url = cff::config::readEnv("DB_URL");
    return url && !url->empty();
}

} // namespace

std::optional<std::string> bearerToken(const drogon::HttpRequestPtr &req) {
    return cff::auth::bearerTokenFromHeader(req->getHeader("authorization"));
}

bool isAuthorized(const drogon::HttpRequestPtr &req,
                  const std::optional<std::string> &secret) {
    const auto token = bearerToken(req);
    if (!token) {
        return false;
    }
#ifdef CFF_HAS_POSTGRES
    if (cff::config::persistentDbRequired() && !databaseConfigured()) {
        return false;
    }
#else
    if (cff::config::persistentDbRequired()) {
        return false;
    }
#endif
    if (cff::config::sharedSecretAuthAllowed() && secret && *token == *secret) {
        return true;
    }
    return cff::auth::emailForSessionToken(*token).has_value();
}

std::optional<std::string> accountEmailForRequest(
    const drogon::HttpRequestPtr &req,
    const std::optional<std::string> &secret) {
    const auto token = bearerToken(req);
    if (!token) {
        return std::nullopt;
    }
    if (cff::config::sharedSecretAuthAllowed() && secret && *token == *secret) {
        return std::string{"admin@example.com"};
    }
    return cff::auth::emailForSessionToken(*token);
}

bool requireAccount(
    const drogon::HttpRequestPtr &req,
    std::function<void(const drogon::HttpResponsePtr &)> &callback,
    const std::optional<std::string> &secret,
    std::string &accountEmail) {
    const auto email = accountEmailForRequest(req, secret);
    if (!email) {
        Json::Value error;
        error["error"] = "Unauthorized";
        auto resp = drogon::HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(drogon::k401Unauthorized);
        callback(resp);
        return false;
    }
    accountEmail = *email;
    return true;
}

bool isAdminRequest(const drogon::HttpRequestPtr &req,
                    const std::optional<std::string> &secret,
                    std::string &adminIdentity) {
    const auto token = bearerToken(req);
    if (!token) {
        return false;
    }

    const auto opsToken = cff::config::readEnv("CFF_ADMIN_API_TOKEN");
    if (opsToken && !opsToken->empty() && *token == *opsToken) {
        adminIdentity = "ops-token";
        return true;
    }

    const auto email = accountEmailForRequest(req, secret);
    if (!email) {
        return false;
    }
    const auto admins = cff::config::csvEmailSetFromEnv("CFF_ADMIN_EMAILS");
    if (!admins.empty()
        && admins.find(cff::auth::canonicalEmail(*email)) != admins.end()) {
        adminIdentity = *email;
        return true;
    }
    adminIdentity = *email;
    return false;
}

bool requireAdmin(
    const drogon::HttpRequestPtr &req,
    std::function<void(const drogon::HttpResponsePtr &)> &callback,
    const std::optional<std::string> &secret,
    std::string &adminIdentity) {
    if (!bearerToken(req)) {
        Json::Value error;
        error["error"] = "Unauthorized";
        auto resp = drogon::HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(drogon::k401Unauthorized);
        callback(resp);
        return false;
    }
    if (!isAdminRequest(req, secret, adminIdentity)) {
        Json::Value error;
        error["error"] = "Admin access required";
        auto resp = drogon::HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(drogon::k403Forbidden);
        callback(resp);
        return false;
    }
    return true;
}

void applyCorsHeaders(
    const drogon::HttpRequestPtr &req,
    const drogon::HttpResponsePtr &resp,
    const std::unordered_set<std::string> &allowedOrigins) {
    if (!req || !resp) {
        return;
    }
    resp->removeHeader("Access-Control-Allow-Origin");
    resp->removeHeader("Access-Control-Allow-Headers");
    resp->removeHeader("Access-Control-Allow-Methods");
    resp->removeHeader("Access-Control-Expose-Headers");
    resp->removeHeader("Access-Control-Max-Age");
    const auto origin = req->getHeader("Origin");
    if (!allowedOrigins.empty() && allowedOrigins.find(origin) != allowedOrigins.end()) {
        resp->addHeader("Access-Control-Allow-Origin", origin);
        resp->addHeader("Vary", "Origin");
    }
    resp->addHeader("Access-Control-Allow-Headers", "Authorization, Content-Type, Idempotency-Key, X-Request-ID");
    resp->addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, PATCH, DELETE, OPTIONS");
    resp->addHeader("Access-Control-Expose-Headers", "X-CFF-Request-Id, Retry-After, X-CFF-Invite-Email");
}

drogon::HttpResponsePtr buildPreflightResponse(
    const drogon::HttpRequestPtr &req,
    const std::unordered_set<std::string> &allowedOrigins) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    applyCorsHeaders(req, resp, allowedOrigins);
    resp->setStatusCode(drogon::k204NoContent);
    return resp;
}

drogon::HttpResponsePtr withRuntimeCorsHeaders(
    const drogon::HttpRequestPtr &req,
    const drogon::HttpResponsePtr &resp) {
    if (resp) {
        applyCorsHeaders(req, resp, cff::config::loadRuntimeConfig().allowedOrigins);
    }
    return resp;
}

} // namespace cff::http
