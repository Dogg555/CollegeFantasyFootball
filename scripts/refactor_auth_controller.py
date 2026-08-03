#!/usr/bin/env python3
"""Extract authentication HTTP handlers from backend/src/main.cpp.

The transformation fails closed whenever an expected source boundary changes.
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


def extract_between(text: str, start: str, end: str, label: str):
    start_index = text.find(start)
    if start_index < 0:
        raise RuntimeError(f"{label}: start marker not found")
    end_index = text.find(end, start_index)
    if end_index < 0:
        raise RuntimeError(f"{label}: end marker not found")
    if text.find(start, start_index + 1) >= 0:
        raise RuntimeError(f"{label}: start marker is not unique")
    return text[start_index:end_index], text[:start_index] + text[end_index:]


main = MAIN.read_text(encoding="utf-8")

main = replace_once(
    main,
    '#include "auth_core.h"\n',
    '#include "auth_core.h"\n#include "auth_controller.h"\n',
    "include auth controller",
)

email_block, main = extract_between(
    main,
    "bool sendEmail(const std::string &to,",
    "#ifdef CFF_HAS_POSTGRES\nstruct PgConnDeleter",
    "extract auth email helpers",
)

credential_block, main = extract_between(
    main,
    "bool ensureCredentials(const Json::Value &body) {",
    "void handleSignup(const drogon::HttpRequestPtr &req,",
    "extract credential request helper",
)

handler_block, main = extract_between(
    main,
    "void handleSignup(const drogon::HttpRequestPtr &req,",
    "std::optional<std::string> getOptionalParam(const drogon::HttpRequestPtr &req,",
    "extract auth handlers",
)

for name in (
    "handleSignup",
    "handleLogin",
    "handleLogout",
    "handleVerifyEmail",
    "handleResendVerification",
    "handleRequestPasswordReset",
    "handleResetPassword",
):
    handler_block = handler_block.replace(f"void {name}(", f"void {name}(", 1)
    main = replace_once(main, f"{name}(req,", f"cff::auth::{name}(req,", f"delegate {name}")

handler_block = handler_block.replace("dbConfigured()", "databaseConfigured()")
handler_block = handler_block.replace("authStorageUnavailable()", "storageUnavailable()")
handler_block = handler_block.replace("issueTokenForUser(", "issueSessionToken(")

for moved_symbol in (
    "bool ensureCredentials(",
    "void handleSignup(",
    "void handleLogin(",
    "void handleLogout(",
    "void handleVerifyEmail(",
    "void handleResendVerification(",
    "void handleRequestPasswordReset(",
    "void handleResetPassword(",
    "bool sendVerificationEmail(",
    "bool sendPasswordResetEmail(",
):
    if moved_symbol in main:
        raise RuntimeError(f"main.cpp still contains moved controller symbol: {moved_symbol}")

MAIN.write_text(main, encoding="utf-8")

HEADER = r'''#pragma once

#include <drogon/drogon.h>

#include <functional>
#include <optional>
#include <string>

namespace cff::auth {

using AuthResponseCallback = std::function<void(const drogon::HttpResponsePtr &)>;

void handleSignup(const drogon::HttpRequestPtr &req,
                  AuthResponseCallback &&callback,
                  const std::optional<std::string> &jwtSecret);
void handleLogin(const drogon::HttpRequestPtr &req,
                 AuthResponseCallback &&callback,
                 const std::optional<std::string> &jwtSecret);
void handleLogout(const drogon::HttpRequestPtr &req,
                  AuthResponseCallback &&callback);
void handleVerifyEmail(const drogon::HttpRequestPtr &req,
                       AuthResponseCallback &&callback);
void handleResendVerification(const drogon::HttpRequestPtr &req,
                              AuthResponseCallback &&callback);
void handleRequestPasswordReset(const drogon::HttpRequestPtr &req,
                                AuthResponseCallback &&callback);
void handleResetPassword(const drogon::HttpRequestPtr &req,
                         AuthResponseCallback &&callback);

} // namespace cff::auth
'''

SOURCE_PREFIX = r'''#include "auth_controller.h"

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

'''

DATABASE_HELPERS = r'''
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

'''

source = SOURCE_PREFIX + email_block + DATABASE_HELPERS + credential_block
source += "} // namespace\n\n" + handler_block + "} // namespace cff::auth\n"

(ROOT / "backend/src/auth_controller.h").write_text(HEADER, encoding="utf-8")
(ROOT / "backend/src/auth_controller.cpp").write_text(source, encoding="utf-8")

BOUNDARY_TEST = r'''#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[2]
main = (root / "backend/src/main.cpp").read_text(encoding="utf-8")
controller = (root / "backend/src/auth_controller.cpp").read_text(encoding="utf-8")
header = (root / "backend/src/auth_controller.h").read_text(encoding="utf-8")

handlers = (
    "handleSignup",
    "handleLogin",
    "handleLogout",
    "handleVerifyEmail",
    "handleResendVerification",
    "handleRequestPasswordReset",
    "handleResetPassword",
)

for handler in handlers:
    if f"void {handler}(" in main:
        raise SystemExit(f"{handler} implementation leaked back into main.cpp")
    if main.count(f"cff::auth::{handler}(req,") != 1:
        raise SystemExit(f"main.cpp must delegate exactly once to {handler}")
    if controller.count(f"void {handler}(") != 1:
        raise SystemExit(f"auth_controller.cpp must define {handler} exactly once")
    if header.count(f"void {handler}(") != 1:
        raise SystemExit(f"auth_controller.h must declare {handler} exactly once")

required_contract_text = (
    "Email and password are required",
    "Account already exists",
    "Invalid credentials",
    "Email verification required",
    "Invalid or expired verification token",
    "If the account exists and needs verification, a verification email will be sent.",
    "If the account exists, a password reset email will be sent.",
    "Password reset. Existing sessions were revoked.",
)
for text in required_contract_text:
    if text not in controller:
        raise SystemExit(f"controller contract text missing: {text}")

if "auth_controller.cpp" not in (root / "backend/CMakeLists.txt").read_text(encoding="utf-8"):
    raise SystemExit("auth_controller.cpp is not part of the production target")

print("authentication controller boundary contracts passed")
'''
(ROOT / "backend/tests/auth_controller_boundary_tests.py").write_text(BOUNDARY_TEST, encoding="utf-8")

cmake = CMAKE.read_text(encoding="utf-8")
cmake = replace_once(
    cmake,
    "    src/auth_core.cpp\n    src/auth_account_store.cpp\n",
    "    src/auth_core.cpp\n    src/auth_controller.cpp\n    src/auth_account_store.cpp\n",
    "add auth controller production source",
)
CMAKE.write_text(cmake, encoding="utf-8")

workflow = WORKFLOW.read_text(encoding="utf-8")
workflow = workflow.replace(
    '      - "backend/src/auth_core.cpp"\n',
    '      - "backend/src/auth_core.cpp"\n      - "backend/src/auth_controller.h"\n      - "backend/src/auth_controller.cpp"\n',
)
if workflow.count('      - "backend/src/auth_controller.cpp"') != 2:
    raise RuntimeError("controller workflow paths were not added to both triggers")
workflow = workflow.replace(
    '      - "backend/tests/auth_core_tests.cpp"\n',
    '      - "backend/tests/auth_core_tests.cpp"\n      - "backend/tests/auth_controller_boundary_tests.py"\n',
)
if workflow.count('      - "backend/tests/auth_controller_boundary_tests.py"') != 2:
    raise RuntimeError("controller boundary test paths were not added to both triggers")
workflow += r'''

  auth-controller-boundary:
    name: Authentication controller boundary contracts
    runs-on: ubuntu-24.04
    timeout-minutes: 5
    steps:
      - uses: actions/checkout@v4
      - uses: actions/setup-python@v5
        with:
          python-version: "3.12"
      - name: Verify controller ownership and route delegation
        run: python backend/tests/auth_controller_boundary_tests.py
'''
WORKFLOW.write_text(workflow, encoding="utf-8")

print("authentication controller extraction applied")
