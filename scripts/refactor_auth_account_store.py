#!/usr/bin/env python3
"""Extract account, verification, and password-reset persistence from main.cpp."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "backend/src/main.cpp"
CMAKE = ROOT / "backend/CMakeLists.txt"


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected one source anchor, found {count}")
    return text.replace(old, new, 1)


def replace_between(text: str, start: str, end: str, replacement: str, label: str) -> str:
    start_index = text.find(start)
    if start_index < 0:
        raise RuntimeError(f"{label}: start marker not found")
    end_index = text.find(end, start_index)
    if end_index < 0:
        raise RuntimeError(f"{label}: end marker not found")
    if text.find(start, start_index + 1) >= 0:
        raise RuntimeError(f"{label}: start marker is not unique")
    return text[:start_index] + replacement + text[end_index:]


main = MAIN.read_text(encoding="utf-8")
main = replace_once(
    main,
    '#include "auth_core.h"\n',
    '#include "auth_core.h"\n#include "auth_account_store.h"\n',
    "include account store",
)
main = replace_once(
    main,
    '''std::mutex userMutex;
std::unordered_map<std::string, std::string> userPasswordHashes;

''',
    "",
    "remove in-memory account state",
)
main = replace_between(
    main,
    "bool dbCreateUser(const std::string &email, const std::string &passwordHash) {",
    "Json::Value dbIngestionStatus() {",
    "",
    "move account persistence functions",
)

replacements = {
    "dbCreateUser(": "cff::auth::createPersistentAccount(",
    "dbPasswordHashForEmail(": "cff::auth::persistentPasswordHashForEmail(",
    "dbEmailVerified(": "cff::auth::persistentEmailVerified(",
    "dbStoreEmailVerificationToken(": "cff::auth::storeEmailVerificationToken(",
    "dbVerifyEmailToken(": "cff::auth::verifyEmailToken(",
    "dbStorePasswordResetToken(": "cff::auth::storePasswordResetToken(",
    "dbResetPassword(": "cff::auth::resetPassword(",
}
for old, new in replacements.items():
    count = main.count(old)
    if count < 1:
        raise RuntimeError(f"expected at least one handler use of {old}")
    main = main.replace(old, new)

main = replace_once(
    main,
    '''    {
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
''',
    '''    if (!cff::auth::createInMemoryAccount(email, *passwordHash)) {
        Json::Value error;
        error["error"] = "Account already exists";
        auto resp = drogon::HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(drogon::k409Conflict);
        callback(resp);
        return;
    }
''',
    "delegate in-memory signup",
)
main = replace_once(
    main,
    '''    {
        std::lock_guard<std::mutex> lock(userMutex);
        auto it = userPasswordHashes.find(email);
        passwordMatches = (it != userPasswordHashes.end() && verifyPassword(password, it->second));
    }
''',
    '''    {
        const auto passwordHash = cff::auth::inMemoryPasswordHashForEmail(email);
        passwordMatches = passwordHash && verifyPassword(password, *passwordHash);
    }
''',
    "delegate in-memory login lookup",
)

for moved_symbol in (
    "userMutex",
    "userPasswordHashes",
    "dbCreateUser",
    "dbPasswordHashForEmail",
    "dbEmailVerified",
    "dbStoreEmailVerificationToken",
    "dbVerifyEmailToken",
    "dbStorePasswordResetToken",
    "dbResetPassword",
):
    if moved_symbol in main:
        raise RuntimeError(f"main.cpp still contains moved symbol: {moved_symbol}")

HEADER = r'''#pragma once

#include <optional>
#include <string>

namespace cff::auth {

bool createInMemoryAccount(const std::string &email, const std::string &passwordHash);
std::optional<std::string> inMemoryPasswordHashForEmail(const std::string &email);

bool createPersistentAccount(const std::string &email, const std::string &passwordHash);
std::optional<std::string> persistentPasswordHashForEmail(const std::string &email);
std::optional<bool> persistentEmailVerified(const std::string &email);
bool storeEmailVerificationToken(const std::string &email, const std::string &token);
std::optional<std::string> verifyEmailToken(const std::string &token);
std::optional<std::string> storePasswordResetToken(const std::string &email,
                                                   const std::string &token);
std::optional<std::string> resetPassword(const std::string &token,
                                         const std::string &passwordHash);

} // namespace cff::auth
'''

SOURCE = r'''#include "auth_account_store.h"

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
'''

TESTS = r'''#include "auth_account_store.h"

#include <iostream>
#include <optional>
#include <string>

namespace {

int failures = 0;

void expect(bool condition, const std::string &message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

} // namespace

int main() {
    using namespace cff::auth;

    expect(!inMemoryPasswordHashForEmail("missing@example.com"),
           "unknown in-memory account is absent");
    expect(createInMemoryAccount("first@example.com", "hash-one"),
           "first in-memory account is created");
    expect(inMemoryPasswordHashForEmail("first@example.com")
               == std::optional<std::string>{"hash-one"},
           "created account password hash is retrievable");
    expect(!createInMemoryAccount("first@example.com", "replacement-hash"),
           "duplicate in-memory account is rejected");
    expect(inMemoryPasswordHashForEmail("first@example.com")
               == std::optional<std::string>{"hash-one"},
           "duplicate creation does not replace the original hash");
    expect(createInMemoryAccount("second@example.com", "hash-two"),
           "second in-memory account is created independently");
    expect(inMemoryPasswordHashForEmail("second@example.com")
               == std::optional<std::string>{"hash-two"},
           "second account retains its own password hash");

    expect(!createPersistentAccount("user@example.com", "hash"),
           "persistent creation fails closed when PostgreSQL support is absent");
    expect(!persistentPasswordHashForEmail("user@example.com"),
           "persistent password lookup fails closed without PostgreSQL");
    expect(!persistentEmailVerified("user@example.com"),
           "verification lookup fails closed without PostgreSQL");
    expect(!storeEmailVerificationToken("user@example.com", "token"),
           "verification-token storage fails closed without PostgreSQL");
    expect(!verifyEmailToken("token"),
           "verification-token consumption fails closed without PostgreSQL");
    expect(!storePasswordResetToken("user@example.com", "token"),
           "reset-token storage fails closed without PostgreSQL");
    expect(!resetPassword("token", "hash"),
           "password reset fails closed without PostgreSQL");

    if (failures != 0) {
        std::cerr << failures << " authentication account assertion(s) failed\n";
        return 1;
    }
    std::cout << "authentication account persistence contracts passed\n";
    return 0;
}
'''

MAIN.write_text(main, encoding="utf-8")
(ROOT / "backend/src/auth_account_store.h").write_text(HEADER, encoding="utf-8")
(ROOT / "backend/src/auth_account_store.cpp").write_text(SOURCE, encoding="utf-8")
(ROOT / "backend/tests/auth_account_store_tests.cpp").write_text(TESTS, encoding="utf-8")

cmake = CMAKE.read_text(encoding="utf-8")
cmake = replace_once(
    cmake,
    "    src/auth_core.cpp\n    src/auth_session_store.cpp\n",
    "    src/auth_core.cpp\n    src/auth_account_store.cpp\n    src/auth_session_store.cpp\n",
    "add production account source",
)
cmake = replace_once(
    cmake,
    '''    target_link_libraries(auth_session_store_tests PRIVATE ${CRYPT_LIB})
    add_test(NAME auth_session_store_tests COMMAND auth_session_store_tests)
endif()
''',
    '''    target_link_libraries(auth_session_store_tests PRIVATE ${CRYPT_LIB})
    add_test(NAME auth_session_store_tests COMMAND auth_session_store_tests)

    add_executable(auth_account_store_tests
        tests/auth_account_store_tests.cpp
        src/auth_account_store.cpp
        src/app_config.cpp
    )
    target_include_directories(auth_account_store_tests PRIVATE src)
    add_test(NAME auth_account_store_tests COMMAND auth_account_store_tests)
endif()
''',
    "register account store tests",
)
CMAKE.write_text(cmake, encoding="utf-8")

print("authentication account store extraction applied")
