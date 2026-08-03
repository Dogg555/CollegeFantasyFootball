#include "auth_account_store.h"

#include "app_config.h"

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

std::mutex accountMutex;
std::unordered_map<std::string, std::string> inMemoryPasswordHashes;

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
#endif

} // namespace

bool createInMemoryAccount(const std::string &email, const std::string &passwordHash) {
    std::lock_guard<std::mutex> lock(accountMutex);
    const auto [it, inserted] = inMemoryPasswordHashes.emplace(email, passwordHash);
    (void)it;
    return inserted;
}

std::optional<std::string> inMemoryPasswordHashForEmail(const std::string &email) {
    std::lock_guard<std::mutex> lock(accountMutex);
    const auto it = inMemoryPasswordHashes.find(email);
    if (it == inMemoryPasswordHashes.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool createPersistentAccount(const std::string &email, const std::string &passwordHash) {
#ifdef CFF_HAS_POSTGRES
    auto conn = connectToDatabase();
    if (!conn) {
        return false;
    }
    auto result = executeParameters(conn.get(),
                                    "INSERT INTO users (email, password_hash) VALUES ($1, $2) ON CONFLICT DO NOTHING",
                                    {email, passwordHash});
    return resultOk(result.get(), PGRES_COMMAND_OK)
        && std::string{PQcmdTuples(result.get())} == "1";
#else
    (void)email;
    (void)passwordHash;
    return false;
#endif
}

std::optional<std::string> persistentPasswordHashForEmail(const std::string &email) {
#ifdef CFF_HAS_POSTGRES
    auto conn = connectToDatabase();
    if (!conn) {
        return std::nullopt;
    }
    auto result = executeParameters(conn.get(),
                                    "SELECT password_hash FROM users WHERE email = $1",
                                    {email});
    if (!resultOk(result.get(), PGRES_TUPLES_OK) || PQntuples(result.get()) == 0) {
        return std::nullopt;
    }
    return std::string{PQgetvalue(result.get(), 0, 0)};
#else
    (void)email;
    return std::nullopt;
#endif
}

std::optional<bool> persistentEmailVerified(const std::string &email) {
#ifdef CFF_HAS_POSTGRES
    auto conn = connectToDatabase();
    if (!conn) {
        return std::nullopt;
    }
    auto result = executeParameters(conn.get(),
                                    "SELECT email_verified FROM users WHERE email = $1",
                                    {email});
    if (!resultOk(result.get(), PGRES_TUPLES_OK) || PQntuples(result.get()) == 0) {
        return std::nullopt;
    }
    return std::string{PQgetvalue(result.get(), 0, 0)} == "t";
#else
    (void)email;
    return std::nullopt;
#endif
}

bool storeEmailVerificationToken(const std::string &email, const std::string &token) {
#ifdef CFF_HAS_POSTGRES
    auto conn = connectToDatabase();
    if (!conn) {
        return false;
    }
    auto result = executeParameters(conn.get(),
                                    "UPDATE users SET email_verification_token = encode(digest($2, 'sha256'), 'hex'), email_verification_expires_at = NOW() + INTERVAL '48 hours', updated_at = NOW() "
                                    "WHERE email = $1 AND email_verified = false",
                                    {email, token});
    return resultOk(result.get(), PGRES_COMMAND_OK)
        && std::string{PQcmdTuples(result.get())} == "1";
#else
    (void)email;
    (void)token;
    return false;
#endif
}

std::optional<std::string> verifyEmailToken(const std::string &token) {
#ifdef CFF_HAS_POSTGRES
    auto conn = connectToDatabase();
    if (!conn) {
        return std::nullopt;
    }
    auto result = executeParameters(conn.get(),
                                    "UPDATE users SET email_verified = true, email_verification_token = NULL, email_verification_expires_at = NULL, updated_at = NOW() "
                                    "WHERE email_verification_token = encode(digest($1, 'sha256'), 'hex') AND email_verification_expires_at > NOW() RETURNING email",
                                    {token});
    if (!resultOk(result.get(), PGRES_TUPLES_OK) || PQntuples(result.get()) == 0) {
        return std::nullopt;
    }
    return std::string{PQgetvalue(result.get(), 0, 0)};
#else
    (void)token;
    return std::nullopt;
#endif
}

std::optional<std::string> storePasswordResetToken(const std::string &email,
                                                   const std::string &token) {
#ifdef CFF_HAS_POSTGRES
    auto conn = connectToDatabase();
    if (!conn) {
        return std::nullopt;
    }
    auto result = executeParameters(conn.get(),
                                    "UPDATE users SET password_reset_token = encode(digest($2, 'sha256'), 'hex'), password_reset_expires_at = NOW() + INTERVAL '1 hour', updated_at = NOW() "
                                    "WHERE email = $1 RETURNING email",
                                    {email, token});
    if (!resultOk(result.get(), PGRES_TUPLES_OK) || PQntuples(result.get()) == 0) {
        return std::nullopt;
    }
    return std::string{PQgetvalue(result.get(), 0, 0)};
#else
    (void)email;
    (void)token;
    return std::nullopt;
#endif
}

std::optional<std::string> resetPassword(const std::string &token,
                                         const std::string &passwordHash) {
#ifdef CFF_HAS_POSTGRES
    auto conn = connectToDatabase();
    if (!conn) {
        return std::nullopt;
    }
    auto result = executeParameters(conn.get(),
                                    "UPDATE users SET password_hash = $2, password_reset_token = NULL, password_reset_expires_at = NULL, updated_at = NOW() "
                                    "WHERE password_reset_token = encode(digest($1, 'sha256'), 'hex') AND password_reset_expires_at > NOW() RETURNING email",
                                    {token, passwordHash});
    if (!resultOk(result.get(), PGRES_TUPLES_OK) || PQntuples(result.get()) == 0) {
        return std::nullopt;
    }
    const auto email = std::string{PQgetvalue(result.get(), 0, 0)};
    auto revoke = executeParameters(conn.get(),
                                    "DELETE FROM auth_tokens WHERE email = $1",
                                    {email});
    (void)revoke;
    return email;
#else
    (void)token;
    (void)passwordHash;
    return std::nullopt;
#endif
}

} // namespace cff::auth
