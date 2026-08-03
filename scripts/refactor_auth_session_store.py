#!/usr/bin/env python3
"""Extract authentication session persistence from backend/src/main.cpp.

The transformation is deliberately fail-closed: every source anchor must match
exactly once before any generated file is written.
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "backend/src/main.cpp"
CMAKE = ROOT / "backend/CMakeLists.txt"
WORKFLOW = ROOT / ".github/workflows/app-config-tests.yml"


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
    '#include "auth_core.h"\n#include "auth_session_store.h"\n',
    "include session store",
)

main = replace_once(
    main,
    '''std::mutex userMutex;
std::unordered_map<std::string, std::string> userPasswordHashes;

struct TokenRecord {
    std::string email;
    std::chrono::steady_clock::time_point expiresAt;
};

std::unordered_map<std::string, TokenRecord> activeTokens; // token -> record
constexpr std::chrono::hours kTokenTtl{24};
''',
    '''std::mutex userMutex;
std::unordered_map<std::string, std::string> userPasswordHashes;
''',
    "remove in-process session state",
)

main = replace_between(
    main,
    "void dbCleanupExpiredTokens(PGconn *conn) {",
    "std::optional<std::string> dbPasswordHashForEmail(const std::string &email) {",
    "",
    "move expired-token cleanup",
)

if main.count("dbRevokeTokensForEmail") != 1:
    raise RuntimeError("unused token revocation helper changed unexpectedly")
main = replace_between(
    main,
    "bool dbPersistToken(const std::string &token, const std::string &email) {",
    "std::optional<bool> dbEmailVerified(const std::string &email) {",
    "",
    "move database session operations",
)

main = replace_between(
    main,
    "std::optional<std::string> issueTokenForUser(const std::string &email) {",
    "bool isAuthorized(const drogon::HttpRequestPtr &req, const std::optional<std::string> &secret) {",
    '''std::optional<std::string> issueTokenForUser(const std::string &email) {
    return cff::auth::issueSessionToken(email);
}

''',
    "delegate token issuance",
)

main = replace_between(
    main,
    "bool isAuthorized(const drogon::HttpRequestPtr &req, const std::optional<std::string> &secret) {",
    "std::optional<std::string> emailForToken(const std::string &token) {",
    '''bool isAuthorized(const drogon::HttpRequestPtr &req, const std::optional<std::string> &secret) {
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
    return cff::auth::emailForSessionToken(token).has_value();
}

''',
    "delegate authorization lookup",
)

main = replace_between(
    main,
    "std::optional<std::string> emailForToken(const std::string &token) {",
    "void applyCorsHeaders(const drogon::HttpRequestPtr &req,",
    '''std::optional<std::string> emailForToken(const std::string &token) {
    return cff::auth::emailForSessionToken(token);
}

''',
    "delegate account lookup",
)

main = replace_once(
    main,
    '''#ifdef CFF_HAS_POSTGRES
    if (dbConfigured()) {
        dbRevokeToken(token);
    }
#endif
    {
        std::lock_guard<std::mutex> lock(userMutex);
        activeTokens.erase(token);
    }
''',
    '''    cff::auth::revokeSessionToken(token);
''',
    "delegate logout revocation",
)

for removed_name in (
    "activeTokens",
    "TokenRecord",
    "kTokenTtl",
    "dbCleanupExpiredTokens",
    "dbPersistToken",
    "dbEmailForToken",
    "dbRevokeToken",
    "dbRevokeTokensForEmail",
):
    if removed_name in main:
        raise RuntimeError(f"main.cpp still contains moved symbol: {removed_name}")

SESSION_HEADER = r'''#pragma once

#include <optional>
#include <string>

namespace cff::auth {

std::optional<std::string> issueSessionToken(const std::string &email);
std::optional<std::string> emailForSessionToken(const std::string &token);
void revokeSessionToken(const std::string &token);

} // namespace cff::auth
'''

SESSION_SOURCE = r'''#include "auth_session_store.h"

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
constexpr std::chrono::hours kTokenTtl{24};

void cleanupExpiredMemoryTokensLocked(std::chrono::steady_clock::time_point now) {
    for (auto it = activeTokens.begin(); it != activeTokens.end();) {
        if (it->second.expiresAt <= now) {
            it = activeTokens.erase(it);
        } else {
            ++it;
        }
    }
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

void revokeDatabaseToken(const std::string &token) {
    auto conn = connectToDatabase();
    if (!conn) {
        return;
    }
    auto result = executeParameters(conn.get(),
                                    "DELETE FROM auth_tokens WHERE token = encode(digest($1, 'sha256'), 'hex')",
                                    {token});
    (void)result;
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
    activeTokens[token] = TokenRecord{email, expiresAt};
    return token;
}

std::optional<std::string> emailForSessionToken(const std::string &token) {
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

void revokeSessionToken(const std::string &token) {
#ifdef CFF_HAS_POSTGRES
    if (databaseConfigured()) {
        revokeDatabaseToken(token);
    }
#endif
    std::lock_guard<std::mutex> lock(sessionMutex);
    activeTokens.erase(token);
}

} // namespace cff::auth
'''

SESSION_TESTS = r'''#include "auth_session_store.h"

#include <cstdlib>
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

void setEnvironment(const char *name, const char *value) {
    if (setenv(name, value, 1) != 0) {
        std::cerr << "Unable to set test environment variable " << name << '\n';
        std::exit(2);
    }
}

void clearEnvironment(const char *name) {
    if (unsetenv(name) != 0) {
        std::cerr << "Unable to clear test environment variable " << name << '\n';
        std::exit(2);
    }
}

} // namespace

int main() {
    using namespace cff::auth;

    clearEnvironment("DB_URL");
    setEnvironment("CFF_REQUIRE_DB", "false");

    const auto first = issueSessionToken("first@example.com");
    expect(first.has_value(), "in-memory session issuance succeeds when persistence is optional");
    if (first) {
        expect(emailForSessionToken(*first) == std::optional<std::string>{"first@example.com"},
               "issued session resolves to its account");
        expect(!emailForSessionToken(*first + "-changed"),
               "modified session token does not resolve");
    }

    const auto second = issueSessionToken("second@example.com");
    expect(second.has_value(), "a second in-memory session can be issued");
    if (first && second) {
        expect(*first != *second, "independent sessions use different tokens");
        expect(emailForSessionToken(*second) == std::optional<std::string>{"second@example.com"},
               "second session remains independently addressable");
        revokeSessionToken(*first);
        expect(!emailForSessionToken(*first), "revoked session no longer resolves");
        expect(emailForSessionToken(*second) == std::optional<std::string>{"second@example.com"},
               "revoking one session does not revoke another account session");
        revokeSessionToken(*second);
        expect(!emailForSessionToken(*second), "second revoked session no longer resolves");
    }

    expect(!emailForSessionToken("token-does-not-exist"),
           "unknown session token is rejected");

    setEnvironment("CFF_REQUIRE_DB", "true");
    expect(!issueSessionToken("required@example.com"),
           "session issuance fails closed when a required database is not compiled or configured");
    expect(!emailForSessionToken("token-anything"),
           "session lookup fails closed when persistent storage is required");

    setEnvironment("CFF_REQUIRE_DB", "false");
    if (failures != 0) {
        std::cerr << failures << " authentication session assertion(s) failed\n";
        return 1;
    }
    std::cout << "authentication session contracts passed\n";
    return 0;
}
'''

MAIN.write_text(main, encoding="utf-8")
(ROOT / "backend/src/auth_session_store.h").write_text(SESSION_HEADER, encoding="utf-8")
(ROOT / "backend/src/auth_session_store.cpp").write_text(SESSION_SOURCE, encoding="utf-8")
(ROOT / "backend/tests/auth_session_store_tests.cpp").write_text(SESSION_TESTS, encoding="utf-8")

cmake = CMAKE.read_text(encoding="utf-8")
cmake = replace_once(
    cmake,
    "    src/auth_core.cpp\n    src/email_delivery.cpp\n",
    "    src/auth_core.cpp\n    src/auth_session_store.cpp\n    src/email_delivery.cpp\n",
    "add production session source",
)
cmake = replace_once(
    cmake,
    '''    target_link_libraries(auth_core_tests PRIVATE ${CRYPT_LIB})
    add_test(NAME auth_core_tests COMMAND auth_core_tests)
endif()
''',
    '''    target_link_libraries(auth_core_tests PRIVATE ${CRYPT_LIB})
    add_test(NAME auth_core_tests COMMAND auth_core_tests)

    add_executable(auth_session_store_tests
        tests/auth_session_store_tests.cpp
        src/auth_session_store.cpp
        src/auth_core.cpp
        src/app_config.cpp
    )
    target_include_directories(auth_session_store_tests PRIVATE src)
    target_link_libraries(auth_session_store_tests PRIVATE ${CRYPT_LIB})
    add_test(NAME auth_session_store_tests COMMAND auth_session_store_tests)
endif()
''',
    "register session store tests",
)
CMAKE.write_text(cmake, encoding="utf-8")

workflow = WORKFLOW.read_text(encoding="utf-8")
workflow = workflow.replace(
    '      - "backend/src/auth_core.cpp"\n',
    '      - "backend/src/auth_core.cpp"\n      - "backend/src/auth_session_store.h"\n      - "backend/src/auth_session_store.cpp"\n',
)
if workflow.count('      - "backend/src/auth_session_store.cpp"') != 2:
    raise RuntimeError("session workflow paths were not added to both triggers")
workflow = workflow.replace(
    '      - "backend/tests/auth_core_tests.cpp"\n',
    '      - "backend/tests/auth_core_tests.cpp"\n      - "backend/tests/auth_session_store_tests.cpp"\n',
)
if workflow.count('      - "backend/tests/auth_session_store_tests.cpp"') != 2:
    raise RuntimeError("session test workflow paths were not added to both triggers")
workflow = replace_once(
    workflow,
    '''      - name: Run authentication core tests
        run: /tmp/auth_core_tests
''',
    '''      - name: Run authentication core tests
        run: /tmp/auth_core_tests

  auth-session-contracts:
    name: Authentication session persistence contracts
    runs-on: ubuntu-24.04
    timeout-minutes: 10
    steps:
      - uses: actions/checkout@v4

      - name: Install crypt development library
        run: |
          sudo apt-get update
          sudo apt-get install -y --no-install-recommends libcrypt-dev

      - name: Compile authentication session tests
        run: |
          g++ -std=c++17 -Wall -Wextra -Werror -pedantic \\
            -Ibackend/src \\
            backend/src/app_config.cpp \\
            backend/src/auth_core.cpp \\
            backend/src/auth_session_store.cpp \\
            backend/tests/auth_session_store_tests.cpp \\
            -lcrypt \\
            -o /tmp/auth_session_store_tests

      - name: Run authentication session tests
        run: /tmp/auth_session_store_tests
''',
    "add session workflow job",
)
WORKFLOW.write_text(workflow, encoding="utf-8")

print("authentication session store extraction applied")
