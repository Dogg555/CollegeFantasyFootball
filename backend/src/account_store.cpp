#include "account_store.h"

#include <postgresql/libpq-fe.h>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace cff::auth {
namespace {

struct PgConnDeleter {
    void operator()(PGconn *connection) const {
        if (connection) PQfinish(connection);
    }
};

struct PgResultDeleter {
    void operator()(PGresult *result) const {
        if (result) PQclear(result);
    }
};

using PgConnPtr = std::unique_ptr<PGconn, PgConnDeleter>;
using PgResultPtr = std::unique_ptr<PGresult, PgResultDeleter>;

PgConnPtr connectToDb() {
    const char *url = std::getenv("DB_URL");
    if (!url || !*url) return nullptr;
    auto connection = PgConnPtr{PQconnectdb(url)};
    if (PQstatus(connection.get()) != CONNECTION_OK) {
        std::cerr << "[auth] account creation database connection failed" << std::endl;
        return nullptr;
    }
    return connection;
}

PgResultPtr execParams(PGconn *connection,
                       const std::string &sql,
                       const std::vector<std::string> &params) {
    std::vector<const char *> values;
    values.reserve(params.size());
    for (const auto &param : params) values.push_back(param.c_str());
    return PgResultPtr{PQexecParams(connection,
                                    sql.c_str(),
                                    static_cast<int>(values.size()),
                                    nullptr,
                                    values.data(),
                                    nullptr,
                                    nullptr,
                                    0)};
}

void logDatabaseFailure(PGresult *result) {
    const char *sqlState = result ? PQresultErrorField(result, PG_DIAG_SQLSTATE) : nullptr;
    std::cerr << "[auth] atomic account insert failed sqlstate="
              << (sqlState ? sqlState : "unknown") << std::endl;
}

}  // namespace

AccountCreateResult createAccount(const std::string &email,
                                  const std::string &passwordHash,
                                  const std::string &verificationToken,
                                  bool verificationRequired) {
    auto connection = connectToDb();
    if (!connection) {
        return {AccountCreateStatus::Unavailable, false};
    }

    const std::string sql = verificationRequired
        ? "INSERT INTO users (email, password_hash, email_verified, email_verification_token, email_verification_expires_at) "
          "VALUES ($1, $2, false, encode(digest($3, 'sha256'), 'hex'), NOW() + INTERVAL '48 hours') "
          "ON CONFLICT (email) DO NOTHING RETURNING email"
        : "INSERT INTO users (email, password_hash, email_verified) "
          "VALUES ($1, $2, false) ON CONFLICT (email) DO NOTHING RETURNING email";

    const auto params = verificationRequired
        ? std::vector<std::string>{email, passwordHash, verificationToken}
        : std::vector<std::string>{email, passwordHash};
    auto result = execParams(connection.get(), sql, params);
    if (!result || PQresultStatus(result.get()) != PGRES_TUPLES_OK) {
        logDatabaseFailure(result.get());
        return {AccountCreateStatus::Failed, false};
    }
    if (PQntuples(result.get()) == 0) {
        return {AccountCreateStatus::AlreadyExists, false};
    }
    return {AccountCreateStatus::Created, verificationRequired};
}

}  // namespace cff::auth
