#include <algorithm>
#include <array>
#include <cstdlib>
#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef DROGON_FOUND
#include <crypt.h>
#include <drogon/drogon.h>
#include <json/json.h>
#ifdef CFF_HAS_POSTGRES
#include <postgresql/libpq-fe.h>
#endif
#include "cfbd_ingest.h"
#include "league_models.h"
#include "handlers/league_handler.h"
#include "player_catalog.h"
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

std::mutex userMutex;
std::unordered_map<std::string, std::string> userPasswordHashes;

struct TokenRecord {
    std::string email;
    std::chrono::steady_clock::time_point expiresAt;
};

std::unordered_map<std::string, TokenRecord> activeTokens; // token -> record
constexpr std::chrono::hours kTokenTtl{24};

bool envFlagEnabled(const std::string &key) {
    const auto value = readEnv(key);
    return value && (*value == "1" || *value == "true" || *value == "TRUE" || *value == "yes" || *value == "YES");
}

bool persistentDbRequired() {
    return envFlagEnabled("CFF_REQUIRE_DB");
}

bool sharedSecretAuthAllowed() {
    return envFlagEnabled("CFF_ALLOW_SHARED_SECRET_AUTH");
}

bool emailVerificationRequired() {
    return envFlagEnabled("CFF_REQUIRE_EMAIL_VERIFICATION");
}

bool exposeAuthTokens() {
    return envFlagEnabled("CFF_EXPOSE_AUTH_TOKENS");
}

bool logAuthTokens() {
    return envFlagEnabled("CFF_LOG_AUTH_TOKENS");
}

#ifdef CFF_HAS_POSTGRES
struct PgConnDeleter {
    void operator()(PGconn *conn) const {
        if (conn) {
            PQfinish(conn);
        }
    }
};

struct PgResultDeleter {
    void operator()(PGresult *result) const {
        if (result) {
            PQclear(result);
        }
    }
};

using PgConnPtr = std::unique_ptr<PGconn, PgConnDeleter>;
using PgResultPtr = std::unique_ptr<PGresult, PgResultDeleter>;

bool dbConfigured() {
    const auto *url = std::getenv("DB_URL");
    return url && std::string{url}.size() > 0;
}

PgConnPtr connectToDb() {
    const auto *url = std::getenv("DB_URL");
    if (!url) {
        return nullptr;
    }
    auto conn = PgConnPtr{PQconnectdb(url)};
    if (PQstatus(conn.get()) != CONNECTION_OK) {
        std::cerr << "[auth] Failed to connect to Postgres: " << PQerrorMessage(conn.get()) << std::endl;
        return nullptr;
    }
    return conn;
}

PgResultPtr execParams(PGconn *conn,
                       const std::string &sql,
                       const std::vector<std::string> &params) {
    std::vector<const char *> values;
    values.reserve(params.size());
    for (const auto &param : params) {
        values.push_back(param.c_str());
    }
    return PgResultPtr{PQexecParams(conn,
                                    sql.c_str(),
                                    static_cast<int>(values.size()),
                                    nullptr,
                                    values.data(),
                                    nullptr,
                                    nullptr,
                                    0)};
}

bool resultOk(PGresult *result, ExecStatusType expected) {
    return result && PQresultStatus(result) == expected;
}

bool dbCreateUser(const std::string &email, const std::string &passwordHash) {
    auto conn = connectToDb();
    if (!conn) return false;
    auto result = execParams(conn.get(),
                             "INSERT INTO users (email, password_hash) VALUES ($1, $2) ON CONFLICT DO NOTHING",
                             {email, passwordHash});
    return resultOk(result.get(), PGRES_COMMAND_OK) && std::string{PQcmdTuples(result.get())} == "1";
}

void dbCleanupExpiredTokens(PGconn *conn) {
    auto cleanupAuth = execParams(conn, "DELETE FROM auth_tokens WHERE expires_at <= NOW()", {});
    (void)cleanupAuth;
    auto cleanupUsers = execParams(conn,
                                   "UPDATE users SET "
                                   "email_verification_token = CASE WHEN email_verification_expires_at <= NOW() THEN NULL ELSE email_verification_token END, "
                                   "email_verification_expires_at = CASE WHEN email_verification_expires_at <= NOW() THEN NULL ELSE email_verification_expires_at END, "
                                   "password_reset_token = CASE WHEN password_reset_expires_at <= NOW() THEN NULL ELSE password_reset_token END, "
                                   "password_reset_expires_at = CASE WHEN password_reset_expires_at <= NOW() THEN NULL ELSE password_reset_expires_at END "
                                   "WHERE email_verification_expires_at <= NOW() OR password_reset_expires_at <= NOW()",
                                   {});
    (void)cleanupUsers;
}

std::optional<std::string> dbPasswordHashForEmail(const std::string &email) {
    auto conn = connectToDb();
    if (!conn) return std::nullopt;
    auto result = execParams(conn.get(),
                             "SELECT password_hash FROM users WHERE email = $1",
                             {email});
    if (!resultOk(result.get(), PGRES_TUPLES_OK) || PQntuples(result.get()) == 0) {
        return std::nullopt;
    }
    return std::string{PQgetvalue(result.get(), 0, 0)};
}

void dbPersistToken(const std::string &token, const std::string &email) {
    auto conn = connectToDb();
    if (!conn) return;
    dbCleanupExpiredTokens(conn.get());
    auto result = execParams(conn.get(),
                             "INSERT INTO auth_tokens (token, email, expires_at) "
                             "VALUES ($1, $2, NOW() + INTERVAL '24 hours') "
                             "ON CONFLICT (token) DO UPDATE SET email = EXCLUDED.email, expires_at = EXCLUDED.expires_at",
                             {token, email});
    if (!resultOk(result.get(), PGRES_COMMAND_OK)) {
        std::cerr << "[auth] token insert failed: " << PQerrorMessage(conn.get()) << std::endl;
    }
}

std::optional<std::string> dbEmailForToken(const std::string &token) {
    auto conn = connectToDb();
    if (!conn) return std::nullopt;
    dbCleanupExpiredTokens(conn.get());
    auto result = execParams(conn.get(),
                             "SELECT email FROM auth_tokens WHERE token = $1 AND expires_at > NOW()",
                             {token});
    if (!resultOk(result.get(), PGRES_TUPLES_OK) || PQntuples(result.get()) == 0) {
        return std::nullopt;
    }
    return std::string{PQgetvalue(result.get(), 0, 0)};
}

void dbRevokeToken(const std::string &token) {
    auto conn = connectToDb();
    if (!conn) return;
    auto result = execParams(conn.get(), "DELETE FROM auth_tokens WHERE token = $1", {token});
    (void)result;
}

void dbRevokeTokensForEmail(const std::string &email) {
    auto conn = connectToDb();
    if (!conn) return;
    auto result = execParams(conn.get(), "DELETE FROM auth_tokens WHERE email = $1", {email});
    (void)result;
}

std::optional<bool> dbEmailVerified(const std::string &email) {
    auto conn = connectToDb();
    if (!conn) return std::nullopt;
    auto result = execParams(conn.get(), "SELECT email_verified FROM users WHERE email = $1", {email});
    if (!resultOk(result.get(), PGRES_TUPLES_OK) || PQntuples(result.get()) == 0) return std::nullopt;
    return std::string{PQgetvalue(result.get(), 0, 0)} == "t";
}

bool dbStoreEmailVerificationToken(const std::string &email, const std::string &token) {
    auto conn = connectToDb();
    if (!conn) return false;
    auto result = execParams(conn.get(),
                             "UPDATE users SET email_verification_token = $2, email_verification_expires_at = NOW() + INTERVAL '48 hours', updated_at = NOW() "
                             "WHERE email = $1 AND email_verified = false",
                             {email, token});
    return resultOk(result.get(), PGRES_COMMAND_OK) && std::string{PQcmdTuples(result.get())} == "1";
}

std::optional<std::string> dbVerifyEmailToken(const std::string &token) {
    auto conn = connectToDb();
    if (!conn) return std::nullopt;
    auto result = execParams(conn.get(),
                             "UPDATE users SET email_verified = true, email_verification_token = NULL, email_verification_expires_at = NULL, updated_at = NOW() "
                             "WHERE email_verification_token = $1 AND email_verification_expires_at > NOW() RETURNING email",
                             {token});
    if (!resultOk(result.get(), PGRES_TUPLES_OK) || PQntuples(result.get()) == 0) return std::nullopt;
    return std::string{PQgetvalue(result.get(), 0, 0)};
}

std::optional<std::string> dbStorePasswordResetToken(const std::string &email, const std::string &token) {
    auto conn = connectToDb();
    if (!conn) return std::nullopt;
    auto result = execParams(conn.get(),
                             "UPDATE users SET password_reset_token = $2, password_reset_expires_at = NOW() + INTERVAL '1 hour', updated_at = NOW() "
                             "WHERE email = $1 RETURNING email",
                             {email, token});
    if (!resultOk(result.get(), PGRES_TUPLES_OK) || PQntuples(result.get()) == 0) return std::nullopt;
    return std::string{PQgetvalue(result.get(), 0, 0)};
}

std::optional<std::string> dbResetPassword(const std::string &token, const std::string &passwordHash) {
    auto conn = connectToDb();
    if (!conn) return std::nullopt;
    auto result = execParams(conn.get(),
                             "UPDATE users SET password_hash = $2, password_reset_token = NULL, password_reset_expires_at = NULL, updated_at = NOW() "
                             "WHERE password_reset_token = $1 AND password_reset_expires_at > NOW() RETURNING email",
                             {token, passwordHash});
    if (!resultOk(result.get(), PGRES_TUPLES_OK) || PQntuples(result.get()) == 0) return std::nullopt;
    const auto email = std::string{PQgetvalue(result.get(), 0, 0)};
    auto revoke = execParams(conn.get(), "DELETE FROM auth_tokens WHERE email = $1", {email});
    (void)revoke;
    return email;
}

Json::Value dbIngestionStatus() {
    Json::Value payload;
    payload["configured"] = dbConfigured();
    payload["runs"] = Json::Value{Json::arrayValue};
    payload["counts"] = Json::Value{Json::objectValue};
    auto conn = connectToDb();
    if (!conn) {
        payload["status"] = "unavailable";
        return payload;
    }

    auto counts = execParams(conn.get(),
                             "SELECT "
                             "(SELECT COUNT(*) FROM teams), "
                             "(SELECT COUNT(*) FROM players), "
                             "(SELECT COUNT(*) FROM games), "
                             "(SELECT COUNT(*) FROM player_stats)",
                             {});
    if (resultOk(counts.get(), PGRES_TUPLES_OK) && PQntuples(counts.get()) > 0) {
        payload["counts"]["teams"] = std::stoll(PQgetvalue(counts.get(), 0, 0));
        payload["counts"]["players"] = std::stoll(PQgetvalue(counts.get(), 0, 1));
        payload["counts"]["games"] = std::stoll(PQgetvalue(counts.get(), 0, 2));
        payload["counts"]["playerStats"] = std::stoll(PQgetvalue(counts.get(), 0, 3));
    }

    auto runs = execParams(conn.get(),
                           "SELECT id, resource, COALESCE(season, 0), COALESCE(week, 0), "
                           "COALESCE(to_char(started_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), ''), "
                           "COALESCE(to_char(finished_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), ''), "
                           "COALESCE(status, ''), call_count, row_count, COALESCE(error_message, '') "
                           "FROM ingestion_runs ORDER BY started_at DESC LIMIT 10",
                           {});
    if (resultOk(runs.get(), PGRES_TUPLES_OK)) {
        for (int row = 0; row < PQntuples(runs.get()); ++row) {
            Json::Value run;
            run["id"] = std::stoll(PQgetvalue(runs.get(), row, 0));
            run["resource"] = PQgetvalue(runs.get(), row, 1);
            run["season"] = std::stoi(PQgetvalue(runs.get(), row, 2));
            run["week"] = std::stoi(PQgetvalue(runs.get(), row, 3));
            run["startedAt"] = PQgetvalue(runs.get(), row, 4);
            run["finishedAt"] = PQgetvalue(runs.get(), row, 5);
            run["status"] = PQgetvalue(runs.get(), row, 6);
            run["apiCalls"] = std::stoi(PQgetvalue(runs.get(), row, 7));
            run["rowCount"] = std::stoi(PQgetvalue(runs.get(), row, 8));
            run["error"] = PQgetvalue(runs.get(), row, 9);
            payload["runs"].append(run);
        }
    }
    payload["status"] = "ok";
    return payload;
}
#endif

Json::Value healthPayload(const std::optional<std::string> &jwtSecret,
                          const std::unordered_set<std::string> &allowedOrigins) {
    Json::Value payload;
    payload["status"] = "ok";
    payload["service"] = "college-ff-api";
    payload["jwtSecretConfigured"] = jwtSecret.has_value();
    payload["allowedOriginsConfigured"] = !allowedOrigins.empty();
    payload["persistentDbRequired"] = persistentDbRequired();
#ifdef CFF_HAS_POSTGRES
    payload["databaseConfigured"] = dbConfigured();
    if (dbConfigured()) {
        auto conn = connectToDb();
        payload["database"] = conn ? "ok" : "unavailable";
        if (!conn && persistentDbRequired()) {
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
    return payload;
}

template <std::size_t N>
bool fillFromUrandom(std::array<unsigned char, N> &bytes) {
    std::ifstream urandom("/dev/urandom", std::ios::in | std::ios::binary);
    if (!urandom.is_open()) {
        return false;
    }
    urandom.read(reinterpret_cast<char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    return urandom.gcount() == static_cast<std::streamsize>(bytes.size());
}


std::optional<std::string> hashPassword(const std::string &password) {
    constexpr int kCost = 12;
    constexpr std::size_t kSaltLen = 16;
    std::array<unsigned char, kSaltLen> saltBytes{};
    if (!fillFromUrandom(saltBytes)) {
        std::random_device rd;
        for (auto &byte : saltBytes) {
            byte = static_cast<unsigned char>(rd());
        }
    }
    char saltBuf[128];
    if (!crypt_gensalt_rn("$2b$", kCost,
                          reinterpret_cast<const char *>(saltBytes.data()),
                          saltBytes.size(),
                          saltBuf, sizeof(saltBuf))) {
        return std::nullopt;
    }
    struct crypt_data data;
    data.initialized = 0;
    const char *hash = crypt_r(password.c_str(), saltBuf, &data);
    if (!hash) {
        return std::nullopt;
    }
    return std::string{hash};
}

bool verifyPassword(const std::string &password, const std::string &hash) {
    struct crypt_data data;
    data.initialized = 0;
    const char *computed = crypt_r(password.c_str(), hash.c_str(), &data);
    if (!computed) {
        return false;
    }
    return hash == computed;
}

bool hasBearerToken(const drogon::HttpRequestPtr &req, std::string &outToken) {
    const auto authHeader = req->getHeader("authorization");
    if (authHeader.size() < 8) {
        return false;
    }
    constexpr std::string_view bearerPrefix = "Bearer ";
    if (authHeader.rfind(bearerPrefix, 0) != 0) {
        return false;
    }
    outToken = authHeader.substr(bearerPrefix.size());
    return true;
}

std::string randomToken() {
    constexpr std::size_t kTokenBytes = 32; // 256 bits of entropy
    std::array<unsigned char, kTokenBytes> bytes{};
    if (!fillFromUrandom(bytes)) {
        std::random_device rd;
        for (auto &b : bytes) {
            b = static_cast<unsigned char>(rd());
        }
    }

    static constexpr char kHex[] = "0123456789abcdef";
    std::string token;
    token.reserve(6 + bytes.size() * 2);
    token.append("token-");
    for (auto byte : bytes) {
        token.push_back(kHex[byte >> 4]);
        token.push_back(kHex[byte & 0x0F]);
    }
    return token;
}

std::string issueTokenForUser(const std::string &email) {
    const auto token = randomToken();
    const auto expiresAt = std::chrono::steady_clock::now() + kTokenTtl;
    std::lock_guard<std::mutex> lock(userMutex);
    const auto now = std::chrono::steady_clock::now();
    for (auto it = activeTokens.begin(); it != activeTokens.end();) {
        if (it->second.expiresAt <= now) {
            it = activeTokens.erase(it);
        } else {
            ++it;
        }
    }
    activeTokens[token] = TokenRecord{email, expiresAt};
#ifdef CFF_HAS_POSTGRES
    if (dbConfigured()) {
        dbPersistToken(token, email);
    }
#endif
    return token;
}

bool isAuthorized(const drogon::HttpRequestPtr &req, const std::optional<std::string> &secret) {
    std::string token;
    if (!hasBearerToken(req, token)) {
        return false;
    }
#ifdef CFF_HAS_POSTGRES
    if (persistentDbRequired() && !dbConfigured()) {
        return false;
    }
#else
    if (persistentDbRequired()) {
        return false;
    }
#endif
    if (sharedSecretAuthAllowed() && secret && token == secret.value()) {
        return true; // compatibility for pre-shared secret
    }
#ifdef CFF_HAS_POSTGRES
    if (dbConfigured()) {
        return dbEmailForToken(token).has_value();
    }
#endif
    std::lock_guard<std::mutex> lock(userMutex);
    const auto now = std::chrono::steady_clock::now();
    auto it = activeTokens.find(token);
    if (it == activeTokens.end()) {
        return false;
    }
    if (it->second.expiresAt <= now) {
        activeTokens.erase(it);
        return false;
    }
    return true;
}

std::optional<std::string> emailForToken(const std::string &token) {
#ifdef CFF_HAS_POSTGRES
    if (persistentDbRequired() && !dbConfigured()) {
        return std::nullopt;
    }
#else
    if (persistentDbRequired()) {
        return std::nullopt;
    }
#endif
#ifdef CFF_HAS_POSTGRES
    if (dbConfigured()) {
        return dbEmailForToken(token);
    }
#endif
    std::lock_guard<std::mutex> lock(userMutex);
    auto it = activeTokens.find(token);
    const auto now = std::chrono::steady_clock::now();
    if (it == activeTokens.end()) {
        return std::nullopt;
    }
    if (it->second.expiresAt <= now) {
        activeTokens.erase(it);
        return std::nullopt;
    }
    return it->second.email;
}

void applyCorsHeaders(const drogon::HttpRequestPtr &req,
                      const drogon::HttpResponsePtr &resp,
                      const std::unordered_set<std::string> &allowedOrigins) {
    const auto origin = req->getHeader("Origin");
    if (!allowedOrigins.empty() && allowedOrigins.find(origin) != allowedOrigins.end()) {
        resp->addHeader("Access-Control-Allow-Origin", origin);
        resp->addHeader("Vary", "Origin");
    }
    resp->addHeader("Access-Control-Allow-Headers", "Authorization, Content-Type");
    resp->addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
}

drogon::HttpResponsePtr buildPreflightResponse(const drogon::HttpRequestPtr &req,
                                               const std::unordered_set<std::string> &allowedOrigins) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    applyCorsHeaders(req, resp, allowedOrigins);
    resp->setStatusCode(drogon::k204NoContent);
    return resp;
}

bool ensureCredentials(const Json::Value &body) {
    constexpr std::size_t kMaxEmail = 254;
    constexpr std::size_t kMinPassword = 8;
    constexpr std::size_t kMaxPassword = 72; // bcrypt truncates longer passwords
    if (!(body.isMember("email") && body["email"].isString()
          && body.isMember("password") && body["password"].isString())) {
        return false;
    }

    const auto email = body["email"].asString();
    const auto password = body["password"].asString();
    if (email.empty() || email.size() > kMaxEmail) {
        return false;
    }
    if (password.size() < kMinPassword || password.size() > kMaxPassword) {
        return false;
    }
    return true;
}

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

    const auto email = (*body)["email"].asString();
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
    if (dbConfigured()) {
        if (!dbCreateUser(email, *passwordHash)) {
            Json::Value error;
            error["error"] = "Account already exists";
            auto resp = drogon::HttpResponse::newHttpJsonResponse(error);
            resp->setStatusCode(drogon::k409Conflict);
            callback(resp);
            return;
        }

        const auto verificationToken = randomToken();
        const bool storedVerification = dbStoreEmailVerificationToken(email, verificationToken);
        if (storedVerification && logAuthTokens()) {
            std::cout << "[auth] email verification token for " << email << ": " << verificationToken << std::endl;
        }
        const auto token = issueTokenForUser(email);
        Json::Value payload;
        payload["email"] = email;
        payload["token"] = token;
        payload["valid"] = true;
        payload["message"] = "Account created";
        payload["emailVerified"] = false;
        payload["emailVerificationRequired"] = emailVerificationRequired();
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

    {
        std::lock_guard<std::mutex> lock(userMutex);
        if (userPasswordHashes.find(email) != userPasswordHashes.end()) {
            Json::Value error;
            error["error"] = "Account already exists";
            auto resp = drogon::HttpResponse::newHttpJsonResponse(error);
            resp->setStatusCode(drogon::k409Conflict);
            callback(resp);
            return;
        }
        userPasswordHashes[email] = *passwordHash;
    }

    const auto token = issueTokenForUser(email);
    Json::Value payload;
    payload["email"] = email;
    payload["token"] = token;
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

    const auto email = (*body)["email"].asString();
    const auto password = (*body)["password"].asString();
    bool passwordMatches = false;
#ifdef CFF_HAS_POSTGRES
    if (dbConfigured()) {
        const auto passwordHash = dbPasswordHashForEmail(email);
        passwordMatches = passwordHash && verifyPassword(password, *passwordHash);
        if (passwordMatches && emailVerificationRequired()) {
            const auto verified = dbEmailVerified(email);
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
        std::lock_guard<std::mutex> lock(userMutex);
        auto it = userPasswordHashes.find(email);
        passwordMatches = (it != userPasswordHashes.end() && verifyPassword(password, it->second));
    }

    if (!passwordMatches) {
        Json::Value error;
        error["error"] = "Invalid credentials";
        auto resp = drogon::HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(drogon::k401Unauthorized);
        callback(resp);
        return;
    }

    const auto token = issueTokenForUser(email);
    Json::Value payload;
    payload["email"] = email;
    payload["token"] = token;
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
#ifdef CFF_HAS_POSTGRES
    if (dbConfigured()) {
        dbRevokeToken(token);
    }
#endif
    {
        std::lock_guard<std::mutex> lock(userMutex);
        activeTokens.erase(token);
    }
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
    if (dbConfigured()) {
        const auto email = dbVerifyEmailToken(token);
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
    const auto email = body && body->isMember("email") && (*body)["email"].isString() ? (*body)["email"].asString() : "";
#ifdef CFF_HAS_POSTGRES
    std::optional<std::string> verificationToken;
    if (dbConfigured() && !email.empty()) {
        verificationToken = randomToken();
        if (dbStoreEmailVerificationToken(email, *verificationToken)) {
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
    const auto email = body && body->isMember("email") && (*body)["email"].isString() ? (*body)["email"].asString() : "";
#ifdef CFF_HAS_POSTGRES
    std::optional<std::string> resetToken;
    if (dbConfigured() && !email.empty()) {
        const auto candidate = randomToken();
        if (dbStorePasswordResetToken(email, candidate)) {
            resetToken = candidate;
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
    if (dbConfigured()) {
        const auto email = dbResetPassword(token, *passwordHash);
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

std::optional<std::string> getOptionalParam(const drogon::HttpRequestPtr &req, const std::string &key) {
    auto value = req->getParameter(key);
    if (value.empty()) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::string> accountEmailForRequest(const drogon::HttpRequestPtr &req,
                                                  const std::optional<std::string> &secret) {
    std::string token;
    if (!hasBearerToken(req, token)) {
        return std::nullopt;
    }
    if (sharedSecretAuthAllowed() && secret && token == secret.value()) {
        return std::string{"admin@example.com"};
    }
    return emailForToken(token);
}

bool requireAccount(const drogon::HttpRequestPtr &req,
                    std::function<void (const drogon::HttpResponsePtr &)> &callback,
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
    const auto ingestOnStartupEnv = readEnv("CFBD_INGEST_ON_STARTUP");

    if (!jwtSecret.has_value()) {
        std::cerr << "[security] JWT_SECRET is not set; secure endpoints will reject all requests." << std::endl;
    }

    // SSL enablement when certs are available
    const bool useSsl = static_cast<bool>(sslCert && sslKey);
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
    } else {
        std::cout << "[security] ALLOWED_ORIGINS not set; cross-origin requests will be blocked." << std::endl;
    }

    app.registerPostHandlingAdvice([allowedOrigins](const drogon::HttpRequestPtr &req,
                                                    const drogon::HttpResponsePtr &resp) {
        applyCorsHeaders(req, resp, allowedOrigins);
    });

    auto preflightHandler = [allowedOrigins](const drogon::HttpRequestPtr &req,
                                             std::function<void (const drogon::HttpResponsePtr &)> &&callback) {
        callback(buildPreflightResponse(req, allowedOrigins));
    };
    auto preflightOneParamHandler = [allowedOrigins](const drogon::HttpRequestPtr &req,
                                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                                     const std::string&) {
        callback(buildPreflightResponse(req, allowedOrigins));
    };
    auto preflightTwoParamHandler = [allowedOrigins](const drogon::HttpRequestPtr &req,
                                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                                     const std::string&,
                                                     const std::string&) {
        callback(buildPreflightResponse(req, allowedOrigins));
    };

    const bool ingestOnStartup = ingestOnStartupEnv &&
                                 (*ingestOnStartupEnv == "1" || *ingestOnStartupEnv == "true" ||
                                  *ingestOnStartupEnv == "TRUE" || *ingestOnStartupEnv == "yes");
    const auto healthHandler = [jwtSecret, allowedOrigins](const drogon::HttpRequestPtr&,
                                                           std::function<void (const drogon::HttpResponsePtr &)> &&callback) {
        auto resp = drogon::HttpResponse::newHttpJsonResponse(healthPayload(jwtSecret, allowedOrigins));
        resp->setStatusCode(drogon::k200OK);
        callback(resp);
    };

    if (ingestOnStartup) {
        std::cout << "[cfbd] CFBD_INGEST_ON_STARTUP enabled; starting ingest..." << std::endl;
        const auto ingestResult = cff::runCfbdIngestOnce();
        std::cout << "[cfbd] ingest complete. inserted=" << ingestResult.ingested
                  << " updated=" << ingestResult.updated
                  << " api_calls=" << ingestResult.apiCalls << std::endl;
        for (const auto &err : ingestResult.errors) {
            std::cerr << "[cfbd] ingest error: " << err << std::endl;
        }
    }

    app.addListener("0.0.0.0", static_cast<unsigned short>(std::stoi(port)), useSsl)
        .setThreadNum(std::thread::hardware_concurrency())
        .registerHandler("/health", healthHandler, {drogon::Get})
        .registerHandler("/api/health", healthHandler, {drogon::Get})
        .registerHandler("/api/secure/ping",
                         [jwtSecret](const drogon::HttpRequestPtr& req, std::function<void (const drogon::HttpResponsePtr &)> &&callback) {
                             auto resp = drogon::HttpResponse::newHttpResponse();
                             if (!isAuthorized(req, jwtSecret)) {
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
                             std::string token;
                             hasBearerToken(req, token);
                             const bool authorized = isAuthorized(req, jwtSecret);
                             Json::Value payload;
                             payload["valid"] = authorized;
                             if (authorized) {
                                 if (auto email = emailForToken(token)) {
                                     payload["email"] = *email;
                                 }
                             }
                             resp->setStatusCode(authorized ? drogon::k200OK : drogon::k401Unauthorized);
                             resp->setBody(payload.toStyledString());
                             resp->addHeader("Content-Type", "application/json");
                             callback(resp);
                         },
                         {drogon::Get})
        .registerHandler("/api/auth/login",
                         [jwtSecret](const drogon::HttpRequestPtr& req, std::function<void (const drogon::HttpResponsePtr &)> &&callback) {
                             handleLogin(req, std::move(callback), jwtSecret);
                         },
                         {drogon::Post})
        .registerHandler("/api/auth/signup",
                         [jwtSecret](const drogon::HttpRequestPtr& req, std::function<void (const drogon::HttpResponsePtr &)> &&callback) {
                             handleSignup(req, std::move(callback), jwtSecret);
                         },
                         {drogon::Post})
        .registerHandler("/api/auth/logout",
                         [](const drogon::HttpRequestPtr& req, std::function<void (const drogon::HttpResponsePtr &)> &&callback) {
                             handleLogout(req, std::move(callback));
                         },
                         {drogon::Post})
        .registerHandler("/api/auth/verify-email",
                         [](const drogon::HttpRequestPtr& req, std::function<void (const drogon::HttpResponsePtr &)> &&callback) {
                             handleVerifyEmail(req, std::move(callback));
                         },
                         {drogon::Post})
        .registerHandler("/api/auth/resend-verification",
                         [](const drogon::HttpRequestPtr& req, std::function<void (const drogon::HttpResponsePtr &)> &&callback) {
                             handleResendVerification(req, std::move(callback));
                         },
                         {drogon::Post})
        .registerHandler("/api/auth/request-password-reset",
                         [](const drogon::HttpRequestPtr& req, std::function<void (const drogon::HttpResponsePtr &)> &&callback) {
                             handleRequestPasswordReset(req, std::move(callback));
                         },
                         {drogon::Post})
        .registerHandler("/api/auth/reset-password",
                         [](const drogon::HttpRequestPtr& req, std::function<void (const drogon::HttpResponsePtr &)> &&callback) {
                             handleResetPassword(req, std::move(callback));
                         },
                         {drogon::Post})
        .registerHandler("/api/admin/ingest/cfbd",
                         [jwtSecret](const drogon::HttpRequestPtr& req, std::function<void (const drogon::HttpResponsePtr &)> &&callback) {
                             if (!isAuthorized(req, jwtSecret)) {
                                 Json::Value error;
                                 error["error"] = "Unauthorized";
                                 auto resp = drogon::HttpResponse::newHttpJsonResponse(error);
                                 resp->setStatusCode(drogon::k401Unauthorized);
                                 callback(resp);
                                 return;
                             }

                             const auto ingestResult = cff::runCfbdIngestOnce();
                             Json::Value payload;
                             payload["status"] = ingestResult.errors.empty() ? "ok" : "partial";
                             payload["ingested"] = static_cast<Json::UInt64>(ingestResult.ingested);
                             payload["updated"] = static_cast<Json::UInt64>(ingestResult.updated);
                             payload["apiCalls"] = static_cast<Json::UInt64>(ingestResult.apiCalls);
                             if (!ingestResult.errors.empty()) {
                                 Json::Value errs(Json::arrayValue);
                                 for (const auto &err : ingestResult.errors) {
                                     errs.append(err);
                                 }
                                 payload["errors"] = errs;
                             }

                             auto resp = drogon::HttpResponse::newHttpJsonResponse(payload);
                             resp->setStatusCode(drogon::k200OK);
                             callback(resp);
                         },
                         {drogon::Post})
        .registerHandler("/api/admin/ingest/cfbd/status",
                         [jwtSecret](const drogon::HttpRequestPtr& req, std::function<void (const drogon::HttpResponsePtr &)> &&callback) {
                             if (!isAuthorized(req, jwtSecret)) {
                                 Json::Value error;
                                 error["error"] = "Unauthorized";
                                 auto resp = drogon::HttpResponse::newHttpJsonResponse(error);
                                 resp->setStatusCode(drogon::k401Unauthorized);
                                 callback(resp);
                                 return;
                             }
#ifndef CFF_HAS_POSTGRES
                             Json::Value payload;
                             payload["configured"] = false;
                             payload["status"] = "unavailable";
                             payload["error"] = "Backend was not built with PostgreSQL support.";
                             auto resp = drogon::HttpResponse::newHttpJsonResponse(payload);
                             resp->setStatusCode(drogon::k503ServiceUnavailable);
                             callback(resp);
#else
                             auto resp = drogon::HttpResponse::newHttpJsonResponse(dbIngestionStatus());
                             resp->setStatusCode(drogon::k200OK);
                             callback(resp);
#endif
                         },
                         {drogon::Get})
        .registerHandler("/api/leagues",
                         [jwtSecret](const drogon::HttpRequestPtr& req, std::function<void (const drogon::HttpResponsePtr &)> &&callback) {
                             std::string accountEmail;
                             if (!requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleListLeagues(req, std::move(callback), accountEmail);
                         },
                         {drogon::Get})
        .registerHandler("/api/leagues",
                         [jwtSecret](const drogon::HttpRequestPtr& req, std::function<void (const drogon::HttpResponsePtr &)> &&callback) {
                             std::string accountEmail;
                             if (!requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleCreateLeague(req, std::move(callback), accountEmail);
                         },
                         {drogon::Post})
        .registerHandler("/api/leagues/{1}",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleGetLeague(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Get})
        .registerHandler("/api/leagues/{1}",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleUpdateLeague(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Put})
        .registerHandler("/api/leagues/{1}",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleDeleteLeague(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Delete})
        .registerHandler("/api/leagues/{1}/members",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleListMembers(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Get})
        .registerHandler("/api/leagues/{1}/members",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleInviteMember(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Post})
        .registerHandler("/api/leagues/{1}/members/{2}",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId,
                                     const std::string &memberEmail) {
                             std::string accountEmail;
                             if (!requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleUpdateMember(req, std::move(callback), accountEmail, leagueId, memberEmail);
                         },
                         {drogon::Put, drogon::Post})
        .registerHandler("/api/leagues/{1}/join",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleJoinLeague(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Post})
        .registerHandler("/api/leagues/{1}/roster",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleGetRoster(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Get})
        .registerHandler("/api/leagues/{1}/rosters/{2}",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId,
                                     const std::string &managerEmail) {
                             std::string accountEmail;
                             if (!requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleGetManagerRoster(req, std::move(callback), accountEmail, leagueId, managerEmail);
                         },
                         {drogon::Get})
        .registerHandler("/api/leagues/{1}/roster",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleAddRosterPlayer(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Post})
        .registerHandler("/api/leagues/{1}/roster/drop",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleDropRosterPlayer(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Post})
        .registerHandler("/api/leagues/{1}/roster/{2}/slot",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId,
                                     const std::string &playerId) {
                             std::string accountEmail;
                             if (!requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleUpdateRosterSlot(req, std::move(callback), accountEmail, leagueId, playerId);
                         },
                         {drogon::Post, drogon::Put})
        .registerHandler("/api/leagues/{1}/free-agents",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleFreeAgents(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Get})
        .registerHandler("/api/leagues/{1}/draft",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleGetDraftState(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Get})
        .registerHandler("/api/leagues/{1}/draft/queue",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleSaveDraftQueue(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Put, drogon::Post})
        .registerHandler("/api/leagues/{1}/draft/picks",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleMakeDraftPick(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Post})
        .registerHandler("/api/leagues/{1}/draft/reset",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleResetDraft(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Post})
        .registerHandler("/api/leagues/{1}/waivers",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleListWaivers(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Get})
        .registerHandler("/api/leagues/{1}/waivers",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleCreateWaiver(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Post})
        .registerHandler("/api/leagues/{1}/waivers/process",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleProcessWaivers(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Post})
        .registerHandler("/api/leagues/{1}/waivers/{2}/process",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId,
                                     const std::string &claimId) {
                             std::string accountEmail;
                             if (!requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleProcessWaiver(req, std::move(callback), accountEmail, leagueId, claimId);
                         },
                         {drogon::Post})
        .registerHandler("/api/leagues/{1}/waivers/{2}/status",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId,
                                     const std::string &claimId) {
                             std::string accountEmail;
                             if (!requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleUpdateWaiverStatus(req, std::move(callback), accountEmail, leagueId, claimId);
                         },
                         {drogon::Post})
        .registerHandler("/api/leagues/{1}/waivers/reorder",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleReorderWaivers(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Post})
        .registerHandler("/api/leagues/{1}/waiver-priority",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleListWaiverPriority(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Get})
        .registerHandler("/api/leagues/{1}/waiver-priority/reset",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleResetWaiverPriority(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Post})
        .registerHandler("/api/leagues/{1}/trades",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleListTrades(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Get})
        .registerHandler("/api/leagues/{1}/trades",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleCreateTrade(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Post})
        .registerHandler("/api/leagues/{1}/trades/{2}/status",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId,
                                     const std::string &tradeId) {
                             std::string accountEmail;
                             if (!requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleUpdateTradeStatus(req, std::move(callback), accountEmail, leagueId, tradeId);
                         },
                         {drogon::Post})
        .registerHandler("/api/leagues/{1}/matchups",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleListMatchups(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Get})
        .registerHandler("/api/leagues/{1}/matchups/generate",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleGenerateMatchups(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Post})
        .registerHandler("/api/leagues/{1}/matchups/generate-season",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleGenerateSeasonSchedule(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Post})
        .registerHandler("/api/leagues/{1}/score/week/{2}",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId,
                                     const std::string &week) {
                             std::string accountEmail;
                             if (!requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleScoreWeek(req, std::move(callback), accountEmail, leagueId, week);
                         },
                         {drogon::Post})
        .registerHandler("/api/leagues/{1}/score/week/{2}/finalize",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId,
                                     const std::string &week) {
                             std::string accountEmail;
                             if (!requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleFinalizeWeek(req, std::move(callback), accountEmail, leagueId, week);
                         },
                         {drogon::Post})
        .registerHandler("/api/leagues/{1}/transactions",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleListTransactions(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Get})
        .registerHandler("/api/scores/live",
                         [](const drogon::HttpRequestPtr&, std::function<void (const drogon::HttpResponsePtr &)> &&callback) {
                             Json::Value payload(Json::arrayValue);
                             Json::Value game;
                             game["away"] = "Oregon";
                             game["home"] = "Ohio State";
                             game["quarter"] = 2;
                             game["clock"] = "07:18";
                             game["awayScore"] = 17;
                             game["homeScore"] = 14;
                             payload.append(game);

                             game.clear();
                             game["away"] = "LSU";
                             game["home"] = "Alabama";
                             game["quarter"] = 3;
                             game["clock"] = "11:02";
                             game["awayScore"] = 24;
                             game["homeScore"] = 24;
                             payload.append(game);

                             auto resp = drogon::HttpResponse::newHttpJsonResponse(payload);
                             resp->setStatusCode(drogon::k200OK);
                             callback(resp);
                         },
                         {drogon::Get})
        .registerHandler("/api/players",
                         [](const drogon::HttpRequestPtr& req, std::function<void (const drogon::HttpResponsePtr &)> &&callback) {
#ifndef CFF_HAS_POSTGRES
                             Json::Value error;
                             error["error"] = "Player search unavailable: backend not built with PostgreSQL support.";
                             auto resp = drogon::HttpResponse::newHttpJsonResponse(error);
                             resp->setStatusCode(drogon::k503ServiceUnavailable);
                             callback(resp);
                             return;
#else
                             const auto query = req->getParameter("query");
                             if (query.empty()) {
                                 Json::Value error;
                                 error["error"] = "Query parameter is required";
                                 auto resp = drogon::HttpResponse::newHttpJsonResponse(error);
                                 resp->setStatusCode(drogon::k400BadRequest);
                                 callback(resp);
                                 return;
                             }

                             auto positionFilter = getOptionalParam(req, "position");
                             auto conferenceFilter = getOptionalParam(req, "conference");

                             std::size_t limit = 25;
                             const auto limitParam = req->getParameter("limit");
                             if (!limitParam.empty()) {
                                 char *end = nullptr;
                                 const auto parsed = std::strtoul(limitParam.c_str(), &end, 10);
                                 if (end != limitParam.c_str() && parsed > 0) {
                                     limit = std::min<std::size_t>(parsed, 50);
                                 }
                             }

                             const auto results = cff::searchPlayers(query, positionFilter, conferenceFilter, limit);
                             Json::Value payload(Json::arrayValue);
                             for (const auto &player : results) {
                                 payload.append(player.toJson());
                             }

                             auto resp = drogon::HttpResponse::newHttpJsonResponse(payload);
                             resp->setStatusCode(drogon::k200OK);
                             callback(resp);
 #endif
                         },
                         {drogon::Get})
        .registerHandler("/api/secure/ping", preflightHandler, {drogon::Options})
        .registerHandler("/api/auth/validate", preflightHandler, {drogon::Options})
        .registerHandler("/api/auth/login", preflightHandler, {drogon::Options})
        .registerHandler("/api/auth/signup", preflightHandler, {drogon::Options})
        .registerHandler("/api/auth/logout", preflightHandler, {drogon::Options})
        .registerHandler("/api/auth/verify-email", preflightHandler, {drogon::Options})
        .registerHandler("/api/auth/resend-verification", preflightHandler, {drogon::Options})
        .registerHandler("/api/auth/request-password-reset", preflightHandler, {drogon::Options})
        .registerHandler("/api/auth/reset-password", preflightHandler, {drogon::Options})
        .registerHandler("/api/leagues", preflightHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}", preflightOneParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/members", preflightOneParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/members/{2}", preflightTwoParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/join", preflightOneParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/roster", preflightOneParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/rosters/{2}", preflightTwoParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/roster/drop", preflightOneParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/roster/{2}/slot", preflightTwoParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/free-agents", preflightOneParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/draft", preflightOneParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/draft/queue", preflightOneParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/draft/picks", preflightOneParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/draft/reset", preflightOneParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/waivers", preflightOneParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/waivers/process", preflightOneParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/waivers/{2}/process", preflightTwoParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/waivers/{2}/status", preflightTwoParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/waivers/reorder", preflightOneParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/waiver-priority", preflightOneParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/waiver-priority/reset", preflightOneParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/trades", preflightOneParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/trades/{2}/status", preflightTwoParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/matchups", preflightOneParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/matchups/generate", preflightOneParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/matchups/generate-season", preflightOneParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/score/week/{2}", preflightTwoParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/score/week/{2}/finalize", preflightTwoParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/transactions", preflightOneParamHandler, {drogon::Options})
        .registerHandler("/api/scores/live", preflightHandler, {drogon::Options})
        .registerHandler("/api/health", preflightHandler, {drogon::Options})
        .registerHandler("/api/admin/ingest/cfbd", preflightHandler, {drogon::Options})
        .registerHandler("/api/admin/ingest/cfbd/status", preflightHandler, {drogon::Options})
        .registerHandler("/api/players", preflightHandler, {drogon::Options})
        .run();
#else
    // Stub output to avoid hard dependency on Drogon in early scaffolding.
    std::cout << "College Fantasy Football backend scaffold (Drogon not linked)." << std::endl;
    std::cout << "Build with -DDROGON_FOUND=ON and link Drogon to run the HTTP server." << std::endl;
#endif
    return 0;
}
