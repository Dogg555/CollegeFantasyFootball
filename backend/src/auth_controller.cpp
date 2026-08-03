#include "auth_controller.h"

#include "app_config.h"
#include "auth_account_store.h"
#include "auth_core.h"
#include "auth_session_store.h"
#include "email_delivery.h"

#include <algorithm>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#ifdef CFF_HAS_POSTGRES
#include <postgresql/libpq-fe.h>
#endif

namespace cff::auth {
namespace {

using cff::config::emailVerificationRequired;
using cff::config::exposeAuthTokens;
using cff::config::frontendBaseUrl;
using cff::config::logAuthTokens;
using cff::config::maxPasswordLength;
using cff::config::minPasswordLength;
using cff::config::persistentDbRequired;
using cff::config::readEnv;

bool sendEmail(const std::string &to,
               const std::string &subject,
               const std::string &text,
               const std::string &html) {
    return cff::sendTransactionalEmail(to, subject, text, html);
}

bool sendVerificationEmail(const std::string &email, const std::string &token) {
    const auto baseUrl = frontendBaseUrl();
    if (!baseUrl) return false;
    const auto link = *baseUrl + "/verify-email.html?token=" + token;
    return sendEmail(email,
                     "Verify your College Fantasy account",
                     "Verify your account: " + link,
                     "<p>Verify your College Fantasy account:</p><p><a href=\"" + link + "\">Verify account</a></p>");
}

bool sendPasswordResetEmail(const std::string &email, const std::string &token) {
    const auto baseUrl = frontendBaseUrl();
    if (!baseUrl) return false;
    const auto link = *baseUrl + "/reset-password.html?token=" + token;
    return sendEmail(email,
                     "Reset your College Fantasy password",
                     "Reset your password: " + link,
                     "<p>Reset your College Fantasy password:</p><p><a href=\"" + link + "\">Reset password</a></p>");
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

bool hasBearerToken(const drogon::HttpRequestPtr &req, std::string &outToken) {
    const auto token = bearerTokenFromHeader(req->getHeader("authorization"));
    if (!token) {
        return false;
    }
    outToken = *token;
    return true;
}

bool ensureCredentials(const Json::Value &body) {
    if (!(body.isMember("email") && body["email"].isString()
          && body.isMember("password") && body["password"].isString())) {
        return false;
    }

    const auto passwordMax = maxPasswordLength();
    const auto passwordMin = std::min(minPasswordLength(), passwordMax);
    return cff::auth::credentialsValid(body["email"].asString(),
                                       body["password"].asString(),
                                       passwordMin,
                                       passwordMax);
}

} // namespace

void handleSignup(const drogon::HttpRequestPtr &req,
                  std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                  const std::optional<std::string> &jwtSecret) {
    (void)jwtSecret;
    const auto body = req->getJsonObject();
    if (!body || !ensureCredentials(*body)) {
        Json::Value error;
        error["error"] = "Email and password are required";
        auto resp = drogon::HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(drogon::k400BadRequest);
        callback(resp);
        return;
    }

    const auto email = canonicalEmail((*body)["email"].asString());
    const auto password = (*body)["password"].asString();
    const auto passwordHash = hashPassword(password);
    if (!passwordHash) {
        Json::Value error;
        error["error"] = "Unable to create account";
        auto resp = drogon::HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(drogon::k500InternalServerError);
        callback(resp);
        return;
    }

#ifdef CFF_HAS_POSTGRES
    if (databaseConfigured()) {
        if (!cff::auth::createPersistentAccount(email, *passwordHash)) {
            if (storageUnavailable()) {
                Json::Value error;
                error["error"] = "Authentication service is temporarily unavailable";
                auto resp = drogon::HttpResponse::newHttpJsonResponse(error);
                resp->setStatusCode(drogon::k503ServiceUnavailable);
                callback(resp);
                return;
            }
            Json::Value error;
            error["error"] = "Account already exists";
            auto resp = drogon::HttpResponse::newHttpJsonResponse(error);
            resp->setStatusCode(drogon::k409Conflict);
            callback(resp);
            return;
        }

        const auto verificationToken = randomToken();
        const bool storedVerification = cff::auth::storeEmailVerificationToken(email, verificationToken);
        const bool sentVerification = storedVerification && sendVerificationEmail(email, verificationToken);
        if (storedVerification && logAuthTokens()) {
            std::cout << "[auth] email verification token for " << email << ": " << verificationToken << std::endl;
        }
        Json::Value payload;
        payload["email"] = email;
        payload["message"] = "Account created";
        payload["emailVerified"] = false;
        payload["emailVerificationRequired"] = emailVerificationRequired();
        payload["emailSent"] = sentVerification;
        if (!emailVerificationRequired()) {
            const auto token = issueSessionToken(email);
            if (!token) {
                Json::Value error;
                error["error"] = "Authentication service is temporarily unavailable";
                auto resp = drogon::HttpResponse::newHttpJsonResponse(error);
                resp->setStatusCode(drogon::k503ServiceUnavailable);
                callback(resp);
                return;
            }
            payload["token"] = *token;
            payload["valid"] = true;
        } else {
            payload["valid"] = false;
            payload["message"] = "Account created. Verify your email before signing in.";
        }
        if (storedVerification && exposeAuthTokens()) {
            payload["emailVerificationToken"] = verificationToken;
        }
        auto resp = drogon::HttpResponse::newHttpJsonResponse(payload);
        resp->setStatusCode(drogon::k201Created);
        callback(resp);
        return;
    }
#endif
    if (persistentDbRequired()) {
        Json::Value error;
        error["error"] = "Database is required but DB_URL is not configured";
        auto resp = drogon::HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(drogon::k503ServiceUnavailable);
        callback(resp);
        return;
    }

    if (!cff::auth::createInMemoryAccount(email, *passwordHash)) {
        Json::Value error;
        error["error"] = "Account already exists";
        auto resp = drogon::HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(drogon::k409Conflict);
        callback(resp);
        return;
    }

    const auto token = issueSessionToken(email);
    Json::Value payload;
    payload["email"] = email;
    if (!token) {
        Json::Value error;
        error["error"] = "Authentication service is temporarily unavailable";
        auto resp = drogon::HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(drogon::k503ServiceUnavailable);
        callback(resp);
        return;
    }
    payload["token"] = *token;
    payload["valid"] = true;
    payload["message"] = "Account created";
    auto resp = drogon::HttpResponse::newHttpJsonResponse(payload);
    resp->setStatusCode(drogon::k201Created);
    callback(resp);
}

void handleLogin(const drogon::HttpRequestPtr &req,
                 std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                 const std::optional<std::string> &jwtSecret) {
    (void)jwtSecret;
    const auto body = req->getJsonObject();
    if (!body || !ensureCredentials(*body)) {
        Json::Value error;
        error["error"] = "Email and password are required";
        auto resp = drogon::HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(drogon::k400BadRequest);
        callback(resp);
        return;
    }

    const auto email = canonicalEmail((*body)["email"].asString());
    const auto password = (*body)["password"].asString();
    bool passwordMatches = false;
#ifdef CFF_HAS_POSTGRES
    if (databaseConfigured()) {
        const auto passwordHash = cff::auth::persistentPasswordHashForEmail(email);
        if (!passwordHash && storageUnavailable()) {
            Json::Value error;
            error["error"] = "Authentication service is temporarily unavailable";
            auto resp = drogon::HttpResponse::newHttpJsonResponse(error);
            resp->setStatusCode(drogon::k503ServiceUnavailable);
            callback(resp);
            return;
        }
        passwordMatches = passwordHash && verifyPassword(password, *passwordHash);
        if (passwordMatches && emailVerificationRequired()) {
            const auto verified = cff::auth::persistentEmailVerified(email);
            if (!verified.value_or(false)) {
                Json::Value error;
                error["error"] = "Email verification required";
                auto resp = drogon::HttpResponse::newHttpJsonResponse(error);
                resp->setStatusCode(drogon::k403Forbidden);
                callback(resp);
                return;
            }
        }
    } else
#endif
    if (persistentDbRequired()) {
        Json::Value error;
        error["error"] = "Database is required but DB_URL is not configured";
        auto resp = drogon::HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(drogon::k503ServiceUnavailable);
        callback(resp);
        return;
    } else
    {
        const auto passwordHash = cff::auth::inMemoryPasswordHashForEmail(email);
        passwordMatches = passwordHash && verifyPassword(password, *passwordHash);
    }

    if (!passwordMatches) {
        Json::Value error;
        error["error"] = "Invalid credentials";
        auto resp = drogon::HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(drogon::k401Unauthorized);
        callback(resp);
        return;
    }

    const auto token = issueSessionToken(email);
    if (!token) {
        Json::Value error;
        error["error"] = "Authentication service is temporarily unavailable";
        auto resp = drogon::HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(drogon::k503ServiceUnavailable);
        callback(resp);
        return;
    }
    Json::Value payload;
    payload["email"] = email;
    payload["token"] = *token;
    payload["valid"] = true;
    payload["message"] = "Signed in";
    auto resp = drogon::HttpResponse::newHttpJsonResponse(payload);
    resp->setStatusCode(drogon::k200OK);
    callback(resp);
}

void handleLogout(const drogon::HttpRequestPtr &req,
                  std::function<void (const drogon::HttpResponsePtr &)> &&callback) {
    std::string token;
    if (!hasBearerToken(req, token)) {
        Json::Value error;
        error["error"] = "Unauthorized";
        auto resp = drogon::HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(drogon::k401Unauthorized);
        callback(resp);
        return;
    }
    cff::auth::revokeSessionToken(token);
    Json::Value payload;
    payload["status"] = "ok";
    auto resp = drogon::HttpResponse::newHttpJsonResponse(payload);
    resp->setStatusCode(drogon::k200OK);
    callback(resp);
}

void handleVerifyEmail(const drogon::HttpRequestPtr &req,
                       std::function<void (const drogon::HttpResponsePtr &)> &&callback) {
    const auto body = req->getJsonObject();
    const auto token = body && body->isMember("token") && (*body)["token"].isString() ? (*body)["token"].asString() : "";
    if (token.empty()) {
        Json::Value error;
        error["error"] = "token is required";
        auto resp = drogon::HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(drogon::k400BadRequest);
        callback(resp);
        return;
    }
#ifdef CFF_HAS_POSTGRES
    if (databaseConfigured()) {
        const auto email = cff::auth::verifyEmailToken(token);
        if (!email) {
            Json::Value error;
            error["error"] = "Invalid or expired verification token";
            auto resp = drogon::HttpResponse::newHttpJsonResponse(error);
            resp->setStatusCode(drogon::k400BadRequest);
            callback(resp);
            return;
        }
        Json::Value payload;
        payload["status"] = "ok";
        payload["email"] = *email;
        payload["emailVerified"] = true;
        auto resp = drogon::HttpResponse::newHttpJsonResponse(payload);
        resp->setStatusCode(drogon::k200OK);
        callback(resp);
        return;
    }
#endif
    Json::Value error;
    error["error"] = "Email verification requires database persistence";
    auto resp = drogon::HttpResponse::newHttpJsonResponse(error);
    resp->setStatusCode(drogon::k503ServiceUnavailable);
    callback(resp);
}

void handleResendVerification(const drogon::HttpRequestPtr &req,
                              std::function<void (const drogon::HttpResponsePtr &)> &&callback) {
    const auto body = req->getJsonObject();
    const auto email = body && body->isMember("email") && (*body)["email"].isString() ? canonicalEmail((*body)["email"].asString()) : "";
#ifdef CFF_HAS_POSTGRES
    std::optional<std::string> verificationToken;
    if (databaseConfigured() && !email.empty()) {
        verificationToken = randomToken();
        if (cff::auth::storeEmailVerificationToken(email, *verificationToken)) {
            sendVerificationEmail(email, *verificationToken);
            if (logAuthTokens()) {
                std::cout << "[auth] email verification token for " << email << ": " << *verificationToken << std::endl;
            }
        } else {
            verificationToken.reset();
        }
    }
#endif
    Json::Value payload;
    payload["status"] = "ok";
    payload["message"] = "If the account exists and needs verification, a verification email will be sent.";
#ifdef CFF_HAS_POSTGRES
    if (verificationToken && exposeAuthTokens()) {
        payload["emailVerificationToken"] = *verificationToken;
    }
#endif
    auto resp = drogon::HttpResponse::newHttpJsonResponse(payload);
    resp->setStatusCode(drogon::k200OK);
    callback(resp);
}

void handleRequestPasswordReset(const drogon::HttpRequestPtr &req,
                                std::function<void (const drogon::HttpResponsePtr &)> &&callback) {
    const auto body = req->getJsonObject();
    const auto email = body && body->isMember("email") && (*body)["email"].isString() ? canonicalEmail((*body)["email"].asString()) : "";
#ifdef CFF_HAS_POSTGRES
    std::optional<std::string> resetToken;
    if (databaseConfigured() && !email.empty()) {
        const auto candidate = randomToken();
        if (cff::auth::storePasswordResetToken(email, candidate)) {
            resetToken = candidate;
            sendPasswordResetEmail(email, candidate);
            if (logAuthTokens()) {
                std::cout << "[auth] password reset token for " << email << ": " << candidate << std::endl;
            }
        }
    }
#endif
    Json::Value payload;
    payload["status"] = "ok";
    payload["message"] = "If the account exists, a password reset email will be sent.";
#ifdef CFF_HAS_POSTGRES
    if (resetToken && exposeAuthTokens()) {
        payload["passwordResetToken"] = *resetToken;
    }
#endif
    auto resp = drogon::HttpResponse::newHttpJsonResponse(payload);
    resp->setStatusCode(drogon::k200OK);
    callback(resp);
}

void handleResetPassword(const drogon::HttpRequestPtr &req,
                         std::function<void (const drogon::HttpResponsePtr &)> &&callback) {
    const auto body = req->getJsonObject();
    const auto token = body && body->isMember("token") && (*body)["token"].isString() ? (*body)["token"].asString() : "";
    const auto password = body && body->isMember("password") && (*body)["password"].isString() ? (*body)["password"].asString() : "";
    Json::Value credentialCheck;
    credentialCheck["email"] = "reset@example.com";
    credentialCheck["password"] = password;
    if (token.empty() || !ensureCredentials(credentialCheck)) {
        Json::Value error;
        error["error"] = "Valid token and password are required";
        auto resp = drogon::HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(drogon::k400BadRequest);
        callback(resp);
        return;
    }
    const auto passwordHash = hashPassword(password);
    if (!passwordHash) {
        Json::Value error;
        error["error"] = "Unable to reset password";
        auto resp = drogon::HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(drogon::k500InternalServerError);
        callback(resp);
        return;
    }
#ifdef CFF_HAS_POSTGRES
    if (databaseConfigured()) {
        const auto email = cff::auth::resetPassword(token, *passwordHash);
        if (!email) {
            Json::Value error;
            error["error"] = "Invalid or expired reset token";
            auto resp = drogon::HttpResponse::newHttpJsonResponse(error);
            resp->setStatusCode(drogon::k400BadRequest);
            callback(resp);
            return;
        }
        Json::Value payload;
        payload["status"] = "ok";
        payload["email"] = *email;
        payload["message"] = "Password reset. Existing sessions were revoked.";
        auto resp = drogon::HttpResponse::newHttpJsonResponse(payload);
        resp->setStatusCode(drogon::k200OK);
        callback(resp);
        return;
    }
#endif
    Json::Value error;
    error["error"] = "Password reset requires database persistence";
    auto resp = drogon::HttpResponse::newHttpJsonResponse(error);
    resp->setStatusCode(drogon::k503ServiceUnavailable);
    callback(resp);
}

} // namespace cff::auth
