#!/usr/bin/env python3
"""Extract stateless authentication primitives from backend/src/main.cpp.

The transformation fails closed when expected source anchors move. It is intended
for one execution on the isolated refactor/extract-auth-core branch.
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "backend/src/main.cpp"
CMAKE = ROOT / "backend/CMakeLists.txt"


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected one source anchor, found {count}")
    return text.replace(old, new, 1)


def remove_between(text: str, start_marker: str, end_marker: str, label: str) -> str:
    start = text.find(start_marker)
    if start < 0:
        raise RuntimeError(f"{label}: start marker not found")
    end = text.find(end_marker, start)
    if end < 0:
        raise RuntimeError(f"{label}: end marker not found")
    return text[:start] + text[end:]


main = MAIN.read_text(encoding="utf-8")

main = replace_once(
    main,
    '#include "app_config.h"\n',
    '#include "auth_core.h"\n#include "app_config.h"\n',
    "include auth core",
)

main = replace_once(
    main,
    '''namespace {
using cff::config::csvEmailSetFromEnv;''',
    '''namespace {
using cff::auth::canonicalEmail;
using cff::auth::hashPassword;
using cff::auth::randomToken;
using cff::auth::verifyPassword;
using cff::config::csvEmailSetFromEnv;''',
    "import auth core functions",
)

main = remove_between(
    main,
    "std::string lowerAscii(std::string value) {",
    "std::string jsonToString(const Json::Value &value) {",
    "remove email canonicalization implementation",
)

main = remove_between(
    main,
    "template <std::size_t N>\nbool fillFromUrandom",
    "bool hasBearerToken(const drogon::HttpRequestPtr &req, std::string &outToken) {",
    "remove password and entropy implementation",
)

has_bearer_start = "bool hasBearerToken(const drogon::HttpRequestPtr &req, std::string &outToken) {"
has_bearer_end = "std::string randomToken() {"
start = main.find(has_bearer_start)
end = main.find(has_bearer_end, start)
if start < 0 or end < 0:
    raise RuntimeError("replace request bearer parser: source anchors not found")
main = main[:start] + '''bool hasBearerToken(const drogon::HttpRequestPtr &req, std::string &outToken) {
    const auto token = cff::auth::bearerTokenFromHeader(req->getHeader("authorization"));
    if (!token) {
        return false;
    }
    outToken = *token;
    return true;
}

''' + main[end:]

main = remove_between(
    main,
    "std::string randomToken() {",
    "std::optional<std::string> issueTokenForUser(const std::string &email) {",
    "remove random token implementation",
)

credentials_start = "bool ensureCredentials(const Json::Value &body) {"
credentials_end = "void handleSignup(const drogon::HttpRequestPtr &req,"
start = main.find(credentials_start)
end = main.find(credentials_end, start)
if start < 0 or end < 0:
    raise RuntimeError("replace credential validation: source anchors not found")
main = main[:start] + '''bool ensureCredentials(const Json::Value &body) {
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

''' + main[end:]

MAIN.write_text(main, encoding="utf-8")

AUTH_HEADER = r'''#pragma once

#include <cstddef>
#include <optional>
#include <string>

namespace cff::auth {

std::string canonicalEmail(std::string value);

bool credentialsValid(const std::string &email,
                      const std::string &password,
                      std::size_t minimumPasswordLength,
                      std::size_t maximumPasswordLength);

std::optional<std::string> hashPassword(const std::string &password);
bool verifyPassword(const std::string &password, const std::string &hash);

std::optional<std::string> bearerTokenFromHeader(const std::string &authorizationHeader);
std::string randomToken();

} // namespace cff::auth
'''

AUTH_SOURCE = r'''#include "auth_core.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <crypt.h>
#include <fstream>
#include <random>
#include <string_view>
#include <utility>

namespace cff::auth {
namespace {

template <std::size_t N>
bool fillFromUrandom(std::array<unsigned char, N> &bytes) {
    std::ifstream urandom("/dev/urandom", std::ios::in | std::ios::binary);
    if (!urandom.is_open()) {
        return false;
    }
    urandom.read(reinterpret_cast<char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    return urandom.gcount() == static_cast<std::streamsize>(bytes.size());
}

} // namespace

std::string canonicalEmail(std::string value) {
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), value.end());
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool credentialsValid(const std::string &email,
                      const std::string &password,
                      std::size_t minimumPasswordLength,
                      std::size_t maximumPasswordLength) {
    constexpr std::size_t kMaximumEmailLength = 254;
    if (email.empty() || email.size() > kMaximumEmailLength) {
        return false;
    }
    return password.size() >= minimumPasswordLength
        && password.size() <= maximumPasswordLength;
}

std::optional<std::string> hashPassword(const std::string &password) {
    constexpr int kCost = 12;
    constexpr std::size_t kSaltLength = 16;
    std::array<unsigned char, kSaltLength> saltBytes{};
    if (!fillFromUrandom(saltBytes)) {
        std::random_device randomDevice;
        for (auto &byte : saltBytes) {
            byte = static_cast<unsigned char>(randomDevice());
        }
    }

    char saltBuffer[128];
    if (!crypt_gensalt_rn("$2b$", kCost,
                          reinterpret_cast<const char *>(saltBytes.data()),
                          saltBytes.size(),
                          saltBuffer, sizeof(saltBuffer))) {
        return std::nullopt;
    }

    struct crypt_data data;
    data.initialized = 0;
    const char *hash = crypt_r(password.c_str(), saltBuffer, &data);
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

std::optional<std::string> bearerTokenFromHeader(const std::string &authorizationHeader) {
    if (authorizationHeader.size() < 8) {
        return std::nullopt;
    }
    constexpr std::string_view kBearerPrefix = "Bearer ";
    if (authorizationHeader.rfind(kBearerPrefix, 0) != 0) {
        return std::nullopt;
    }
    return authorizationHeader.substr(kBearerPrefix.size());
}

std::string randomToken() {
    constexpr std::size_t kTokenBytes = 32;
    std::array<unsigned char, kTokenBytes> bytes{};
    if (!fillFromUrandom(bytes)) {
        std::random_device randomDevice;
        for (auto &byte : bytes) {
            byte = static_cast<unsigned char>(randomDevice());
        }
    }

    static constexpr char kHex[] = "0123456789abcdef";
    std::string token;
    token.reserve(6 + bytes.size() * 2);
    token.append("token-");
    for (const auto byte : bytes) {
        token.push_back(kHex[byte >> 4]);
        token.push_back(kHex[byte & 0x0F]);
    }
    return token;
}

} // namespace cff::auth
'''

AUTH_TESTS = r'''#include "auth_core.h"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void expect(bool condition, const std::string &message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

bool isLowerHexToken(const std::string &token) {
    if (token.size() != 70 || token.rfind("token-", 0) != 0) {
        return false;
    }
    return std::all_of(token.begin() + 6, token.end(), [](unsigned char ch) {
        return std::isdigit(ch) || (ch >= 'a' && ch <= 'f');
    });
}

} // namespace

int main() {
    using namespace cff::auth;

    expect(canonicalEmail("  User.Name+Tag@Example.COM \t") == "user.name+tag@example.com",
           "email canonicalization trims and lowercases");
    expect(canonicalEmail("A B@EXAMPLE.COM") == "a b@example.com",
           "email canonicalization preserves existing internal characters");

    expect(credentialsValid("user@example.com", std::string(12, 'x'), 12, 72),
           "minimum-length password accepted");
    expect(!credentialsValid("", std::string(12, 'x'), 12, 72),
           "empty email rejected");
    expect(!credentialsValid(std::string(255, 'a'), std::string(12, 'x'), 12, 72),
           "oversized email rejected");
    expect(!credentialsValid("user@example.com", std::string(11, 'x'), 12, 72),
           "short password rejected");
    expect(!credentialsValid("user@example.com", std::string(73, 'x'), 12, 72),
           "password above bcrypt policy rejected");

    expect(bearerTokenFromHeader("Bearer abc123") == std::optional<std::string>{"abc123"},
           "bearer token extracted");
    expect(!bearerTokenFromHeader("bearer abc123"), "bearer prefix remains case-sensitive");
    expect(!bearerTokenFromHeader("Bearer "), "empty bearer token rejected");
    expect(!bearerTokenFromHeader("abc123"), "non-bearer header rejected");

    const auto firstToken = randomToken();
    const auto secondToken = randomToken();
    expect(isLowerHexToken(firstToken), "generated token retains prefix, entropy length, and encoding");
    expect(isLowerHexToken(secondToken), "second generated token retains format");
    expect(firstToken != secondToken, "independent generated tokens differ");

    const auto passwordHash = hashPassword("correct horse battery staple");
    expect(passwordHash.has_value(), "bcrypt password hash generated");
    if (passwordHash) {
        expect(passwordHash->rfind("$2b$12$", 0) == 0, "bcrypt cost and format retained");
        expect(verifyPassword("correct horse battery staple", *passwordHash),
               "correct password verifies");
        expect(!verifyPassword("incorrect password", *passwordHash),
               "incorrect password rejected");
    }

    if (failures != 0) {
        std::cerr << failures << " authentication core assertion(s) failed\n";
        return 1;
    }
    std::cout << "authentication core contracts passed\n";
    return 0;
}
'''

(ROOT / "backend/src/auth_core.h").write_text(AUTH_HEADER, encoding="utf-8")
(ROOT / "backend/src/auth_core.cpp").write_text(AUTH_SOURCE, encoding="utf-8")
(ROOT / "backend/tests/auth_core_tests.cpp").write_text(AUTH_TESTS, encoding="utf-8")

cmake = CMAKE.read_text(encoding="utf-8")
cmake = replace_once(
    cmake,
    "    src/app_config.cpp\n",
    "    src/app_config.cpp\n    src/auth_core.cpp\n",
    "add auth core production source",
)
cmake = replace_once(
    cmake,
    '''    target_include_directories(app_config_tests PRIVATE src)
    add_test(NAME app_config_tests COMMAND app_config_tests)
endif()
''',
    '''    target_include_directories(app_config_tests PRIVATE src)
    add_test(NAME app_config_tests COMMAND app_config_tests)

    add_executable(auth_core_tests
        tests/auth_core_tests.cpp
        src/auth_core.cpp
    )
    target_include_directories(auth_core_tests PRIVATE src)
    target_link_libraries(auth_core_tests PRIVATE ${CRYPT_LIB})
    add_test(NAME auth_core_tests COMMAND auth_core_tests)
endif()
''',
    "register auth core tests",
)
CMAKE.write_text(cmake, encoding="utf-8")

print("authentication core extraction applied")
