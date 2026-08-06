#include "auth_session_store.h"

#include "app_config.h"
#include "auth_core.h"

#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef CFF_HAS_POSTGRES
#include <postgresql/libpq-fe.h>
#endif

namespace cff::auth {
namespace {

struct TokenRecord {
    std::string email;
    std::chrono::steady_clock::time_point expiresAt;
};

std::mutex sessionMutex;
std::unordered_map<std::string, TokenRecord> activeTokens;
std::unordered_map<std::string, std::chrono::steady_clock::time_point> revokedTokens;
constexpr std::chrono::hours kTokenTtl{24};

void cleanupExpiredMemoryTokensLocked(std::chrono::steady_clock::time_point now) {
    for (auto it = activeTokens.begin(); it != activeTokens.end();) {
        if (it->second.expiresAt <= now) {
            it = activeTokens.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = revokedTokens.begin(); it != revokedTokens.end();) {
        if (it->second <= now) {
            it = revokedTokens.erase(it);
        } else {
            ++it;
        }
    }
}

bool tokenRevokedInMemory(const std::string &token) {
    std::lock_guard<std::mutex> lock(sessionMutex);
    const auto now = std::chrono::steady_clock::now();
    cleanupExpiredMemoryTokensLocked(now);
    return revokedTokens.find(token) != revokedTokens.end();
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
        std::cerr << "[auth] Failed to connect to Postgres: " << PQerrorMessage(conn.get()) << std::endl;
        return nullptr;
    }
    return conn;
}

PgResultPtr executeParameters(PGconn *conn,
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

void cleanupExpiredDatabaseTokens(PGconn *conn) {
    auto cleanupAuth = executeParameters(conn, "DELETE FROM auth_tokens WHERE expires_at <= NOW()", {});
    (void)cleanupAuth;
    auto cleanupUsers = executeParameters(conn,
                                          "UPDATE users SET "
                                          "email_verification_token = CASE WHEN email_verification_expires_at <= NOW() THEN NULL ELSE email_verification_token END, "
                                          "email_verification_expires_at = CASE WHEN email_verification_expires_at <= NOW() THEN NULL ELSE email_verification_expires_at END, "
                                          "password_reset_token = CASE WHEN password_reset_expires_at <= NOW() THEN NULL ELSE password_reset_token END, "
                                          "password_reset_expires_at = CASE WHEN password_reset_expires_at <= NOW() THEN NULL ELSE password_reset_expires_at END "
                                          "WHERE email_verification_expires_at <= NOW() OR password_reset_expires_at <= NOW()",
                                          {});
    (void)cleanupUsers;
}

bool persistDatabaseToken(const std::string &token, const std::string &email) {
    auto conn = connectToDatabase();
    if (!conn) {
        return false;
    }
    cleanupExpiredDatabaseTokens(conn.get());
    auto result = executeParameters(conn.get(),
                                    "INSERT INTO auth_tokens (token, email, expires_at) "
                                    "VALUES (encode(digest($1, 'sha256'), 'hex'), $2, NOW() + INTERVAL '24 hours') "
                                    "ON CONFLICT (token) DO UPDATE SET email = EXCLUDED.email, expires_at = EXCLUDED.expires_at",
                                    {token, email});
    if (!resultOk(result.get(), PGRES_COMMAND_OK)) {
        std::cerr << "[auth] token insert failed: " << PQerrorMessage(conn.get()) << std::endl;
        return false;
    }
    return true;
}

std::optional<std::string> databaseEmailForToken(const std::string &token) {
    auto conn = connectToDatabase();
    if (!conn) {
        return std::nullopt;
    }
    cleanupExpiredDatabaseTokens(conn.get());
    auto result = executeParameters(conn.get(),
                                    "SELECT email FROM auth_tokens WHERE token = encode(digest($1, 'sha256'), 'hex') AND expires_at > NOW()",
                                    {token});
    if (!resultOk(result.get(), PGRES_TUPLES_OK) || PQntuples(result.get()) == 0) {
        return std::nullopt;
    }
    return std::string{PQgetvalue(result.get(), 0, 0)};
}

bool revokeDatabaseToken(const std::string &token) {
    auto conn = connectToDatabase();
    if (!conn) {
        return false;
    }
    auto result = executeParameters(conn.get(),
                                    "DELETE FROM auth_tokens WHERE token = encode(digest($1, 'sha256'), 'hex')",
                                    {token});
    if (!resultOk(result.get(), PGRES_COMMAND_OK)) {
        std::cerr << "[auth] token revocation failed: " << PQerrorMessage(conn.get()) << std::endl;
        return false;
    }
    return true;
}
#endif

} // namespace

std::optional<std::string> issueSessionToken(const std::string &email) {
    const auto token = randomToken();
    const auto expiresAt = std::chrono::steady_clock::now() + kTokenTtl;
#ifdef CFF_HAS_POSTGRES
    if (databaseConfigured()) {
        if (!persistDatabaseToken(token, email)) {
            return std::nullopt;
        }
    } else if (cff::config::persistentDbRequired()) {
        return std::nullopt;
    }
#else
    if (cff::config::persistentDbRequired()) {
        return std::nullopt;
    }
#endif

    std::lock_guard<std::mutex> lock(sessionMutex);
    cleanupExpiredMemoryTokensLocked(std::chrono::steady_clock::now());
    revokedTokens.erase(token);
    activeTokens[token] = TokenRecord{email, expiresAt};
    return token;
}

std::optional<std::string> emailForSessionToken(const std::string &token) {
    if (tokenRevokedInMemory(token)) {
        return std::nullopt;
    }
#ifdef CFF_HAS_POSTGRES
    if (cff::config::persistentDbRequired() && !databaseConfigured()) {
        return std::nullopt;
    }
    if (databaseConfigured()) {
        return databaseEmailForToken(token);
    }
#else
    if (cff::config::persistentDbRequired()) {
        return std::nullopt;
    }
#endif

    std::lock_guard<std::mutex> lock(sessionMutex);
    const auto now = std::chrono::steady_clock::now();
    cleanupExpiredMemoryTokensLocked(now);
    auto it = activeTokens.find(token);
    if (it == activeTokens.end()) {
        return std::nullopt;
    }
    if (it->second.expiresAt <= now) {
        activeTokens.erase(it);
        return std::nullopt;
    }
    return it->second.email;
}

bool revokeSessionToken(const std::string &token) {
    bool persistentRevocationConfirmed = true;
    {
        std::lock_guard<std::mutex> lock(sessionMutex);
        const auto now = std::chrono::steady_clock::now();
        cleanupExpiredMemoryTokensLocked(now);
        activeTokens.erase(token);
        revokedTokens[token] = now + kTokenTtl;
    }
#ifdef CFF_HAS_POSTGRES
    if (databaseConfigured()) {
        persistentRevocationConfirmed = revokeDatabaseToken(token);
        if (!persistentRevocationConfirmed) {
            std::cerr << "[auth] persistent token revocation could not be confirmed; "
                      << "the token remains denied in this process" << std::endl;
        }
    } else if (cff::config::persistentDbRequired()) {
        persistentRevocationConfirmed = false;
    }
#else
    if (cff::config::persistentDbRequired()) {
        persistentRevocationConfirmed = false;
    }
#endif
    return persistentRevocationConfirmed;
}

} // namespace cff::auth
