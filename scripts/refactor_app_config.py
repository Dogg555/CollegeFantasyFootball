#!/usr/bin/env python3
"""Apply the behavior-preserving app configuration extraction.

This script is executed once by a temporary GitHub Actions workflow because the
maintenance environment does not have direct git network access. It uses exact
source anchors and fails closed if the expected Test-branch source has moved.
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


main = MAIN.read_text(encoding="utf-8")

main = replace_once(
    main,
    '#include "cfbd_ingest.h"\n#include "email_delivery.h"',
    '#include "app_config.h"\n#include "cfbd_ingest.h"\n#include "email_delivery.h"',
    "include app_config",
)

main = replace_once(
    main,
    '''namespace {
std::optional<std::string> readEnv(const std::string &key) {
    const char *val = std::getenv(key.c_str());
    if (val == nullptr) {
        return std::nullopt;
    }
    return std::string{val};
}

std::mutex userMutex;''',
    '''namespace {
using cff::config::csvEmailSetFromEnv;
using cff::config::emailVerificationRequired;
using cff::config::exposeAuthTokens;
using cff::config::frontendBaseUrl;
using cff::config::logAuthTokens;
using cff::config::maxPasswordLength;
using cff::config::minPasswordLength;
using cff::config::persistentDbRequired;
using cff::config::readEnv;
using cff::config::sharedSecretAuthAllowed;

std::mutex userMutex;''',
    "replace local readEnv with config imports",
)

main = replace_once(
    main,
    '''bool envFlagEnabled(const std::string &key) {
    const auto value = readEnv(key);
    return value && (*value == "1" || *value == "true" || *value == "TRUE" || *value == "yes" || *value == "YES");
}

std::optional<int> readPositiveIntEnv(const std::string &key) {
    const auto value = readEnv(key);
    if (!value || value->empty()) {
        return std::nullopt;
    }
    char *end = nullptr;
    const long parsed = std::strtol(value->c_str(), &end, 10);
    if (end == value->c_str() || parsed <= 0 || parsed > 24 * 30) {
        return std::nullopt;
    }
    return static_cast<int>(parsed);
}

std::size_t readSizeEnv(const std::string &key, std::size_t fallback, std::size_t maximum) {
    const auto value = readEnv(key);
    if (!value || value->empty()) {
        return fallback;
    }
    char *end = nullptr;
    const unsigned long parsed = std::strtoul(value->c_str(), &end, 10);
    if (end == value->c_str() || parsed == 0 || parsed > maximum) {
        return fallback;
    }
    return static_cast<std::size_t>(parsed);
}

bool persistentDbRequired() {
    return envFlagEnabled("CFF_REQUIRE_DB");
}

std::size_t minPasswordLength() {
    return readSizeEnv("CFF_MIN_PASSWORD_LENGTH", 12, 72);
}

std::size_t maxPasswordLength() {
    return readSizeEnv("CFF_MAX_PASSWORD_LENGTH", 72, 72);
}

''',
    "",
    "remove primitive configuration helpers",
)

main = replace_once(
    main,
    '''bool sharedSecretAuthAllowed() {
    return envFlagEnabled("CFF_ALLOW_SHARED_SECRET_AUTH");
}

''',
    "",
    "remove shared secret config helper",
)

main = replace_once(
    main,
    '''std::unordered_set<std::string> csvEmailSetFromEnv(const std::string &key) {
    std::unordered_set<std::string> values;
    const auto raw = readEnv(key);
    if (!raw) return values;
    std::size_t start = 0;
    while (start <= raw->size()) {
        const auto pos = raw->find(',', start);
        auto item = raw->substr(start, pos == std::string::npos ? std::string::npos : pos - start);
        item.erase(item.begin(), std::find_if(item.begin(), item.end(), [](unsigned char ch) {
            return !std::isspace(ch);
        }));
        item.erase(std::find_if(item.rbegin(), item.rend(), [](unsigned char ch) {
            return !std::isspace(ch);
        }).base(), item.end());
        if (!item.empty()) {
            values.insert(lowerAscii(item));
        }
        if (pos == std::string::npos) break;
        start = pos + 1;
    }
    return values;
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

''',
    "",
    "remove email and auth configuration helpers",
)

main = replace_once(
    main,
    '''std::optional<std::string> frontendBaseUrl() {
    if (auto url = readEnv("CFF_FRONTEND_BASE_URL")) {
        if (!url->empty()) return url;
    }
    if (auto origins = readEnv("ALLOWED_ORIGINS")) {
        const auto comma = origins->find(',');
        auto first = origins->substr(0, comma == std::string::npos ? std::string::npos : comma);
        while (!first.empty() && first.back() == '/') first.pop_back();
        if (!first.empty()) return first;
    }
    return std::nullopt;
}

''',
    "",
    "remove frontend URL configuration helper",
)

main = replace_once(
    main,
    '''bool dbConfigured() {
    const auto *url = std::getenv("DB_URL");
    return url && std::string{url}.size() > 0;
}

PgConnPtr connectToDb() {
    const auto *url = std::getenv("DB_URL");
    if (!url) {
        return nullptr;
    }
    auto conn = PgConnPtr{PQconnectdb(url)};''',
    '''bool dbConfigured() {
    const auto url = readEnv("DB_URL");
    return url && !url->empty();
}

PgConnPtr connectToDb() {
    const auto url = readEnv("DB_URL");
    if (!url || url->empty()) {
        return nullptr;
    }
    auto conn = PgConnPtr{PQconnectdb(url->c_str())};''',
    "route DB_URL through app config",
)

main = replace_once(
    main,
    '''    // Environment configuration
    const auto port = readEnv("PORT").value_or("8080");
    const auto jwtSecret = readEnv("JWT_SECRET");
    const auto sslCert = readEnv("SSL_CERT_FILE");
    const auto sslKey = readEnv("SSL_KEY_FILE");
    const auto allowedOriginEnv = readEnv("ALLOWED_ORIGINS");
    const auto ingestOnStartupEnv = readEnv("CFBD_INGEST_ON_STARTUP");
    const auto ingestIntervalHours = readPositiveIntEnv("CFBD_INGEST_INTERVAL_HOURS");''',
    '''    // Load environment configuration once at startup. Policy helpers used by
    // request handlers remain centralized in app_config.cpp.
    const auto runtimeConfig = cff::config::loadRuntimeConfig();
    const auto &port = runtimeConfig.port;
    const auto &jwtSecret = runtimeConfig.jwtSecret;
    const auto &sslCert = runtimeConfig.sslCert;
    const auto &sslKey = runtimeConfig.sslKey;
    const auto &ingestIntervalHours = runtimeConfig.ingestIntervalHours;''',
    "load RuntimeConfig in main",
)

main = replace_once(
    main,
    '    const bool useSsl = static_cast<bool>(sslCert && sslKey);',
    '    const bool useSsl = runtimeConfig.sslEnabled();',
    "use RuntimeConfig SSL policy",
)

main = replace_once(
    main,
    '''    // Minimal CORS handling via post-routing advice
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
''',
    '''    // Minimal CORS handling via post-routing advice
    const auto &allowedOrigins = runtimeConfig.allowedOrigins;
    if (!runtimeConfig.allowedOriginsConfigured) {
        std::cout << "[security] ALLOWED_ORIGINS not set; cross-origin requests will be blocked." << std::endl;
    }
''',
    "use RuntimeConfig CORS origins",
)

main = replace_once(
    main,
    '''    const bool ingestOnStartup = ingestOnStartupEnv &&
                                 (*ingestOnStartupEnv == "1" || *ingestOnStartupEnv == "true" ||
                                  *ingestOnStartupEnv == "TRUE" || *ingestOnStartupEnv == "yes");''',
    '    const bool ingestOnStartup = runtimeConfig.ingestOnStartup;',
    "use RuntimeConfig ingest policy",
)

MAIN.write_text(main, encoding="utf-8")

app_config_h = r'''#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_set>

namespace cff::config {

struct RuntimeConfig {
    std::string port{"8080"};
    std::optional<std::string> jwtSecret;
    std::optional<std::string> sslCert;
    std::optional<std::string> sslKey;
    std::unordered_set<std::string> allowedOrigins;
    bool allowedOriginsConfigured{false};
    bool ingestOnStartup{false};
    std::optional<int> ingestIntervalHours;

    bool sslEnabled() const;
};

std::optional<std::string> readEnv(const std::string &key);
bool envFlagEnabled(const std::string &key);
std::optional<int> readPositiveIntEnv(const std::string &key);
std::size_t readSizeEnv(const std::string &key,
                        std::size_t fallback,
                        std::size_t maximum);

bool persistentDbRequired();
std::size_t minPasswordLength();
std::size_t maxPasswordLength();
bool sharedSecretAuthAllowed();
std::unordered_set<std::string> csvEmailSetFromEnv(const std::string &key);
bool emailVerificationRequired();
bool exposeAuthTokens();
bool logAuthTokens();
std::optional<std::string> frontendBaseUrl();
RuntimeConfig loadRuntimeConfig();

} // namespace cff::config
'''

app_config_cpp = r'''#include "app_config.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <utility>

namespace cff::config {
namespace {

std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string trimAscii(std::string value) {
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), value.end());
    return value;
}

std::unordered_set<std::string> splitOrigins(const std::string &raw) {
    std::unordered_set<std::string> values;
    std::size_t start = 0;
    while (true) {
        const auto pos = raw.find(',', start);
        auto value = raw.substr(start, pos == std::string::npos ? std::string::npos : pos - start);
        if (!value.empty()) {
            values.insert(std::move(value));
        }
        if (pos == std::string::npos) {
            break;
        }
        start = pos + 1;
    }
    return values;
}

bool ingestOnStartupEnabled() {
    const auto value = readEnv("CFBD_INGEST_ON_STARTUP");
    return value && (*value == "1" || *value == "true" ||
                     *value == "TRUE" || *value == "yes");
}

} // namespace

bool RuntimeConfig::sslEnabled() const {
    return sslCert.has_value() && sslKey.has_value();
}

std::optional<std::string> readEnv(const std::string &key) {
    const char *value = std::getenv(key.c_str());
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string{value};
}

bool envFlagEnabled(const std::string &key) {
    const auto value = readEnv(key);
    return value && (*value == "1" || *value == "true" || *value == "TRUE" ||
                     *value == "yes" || *value == "YES");
}

std::optional<int> readPositiveIntEnv(const std::string &key) {
    const auto value = readEnv(key);
    if (!value || value->empty()) {
        return std::nullopt;
    }
    char *end = nullptr;
    const long parsed = std::strtol(value->c_str(), &end, 10);
    if (end == value->c_str() || parsed <= 0 || parsed > 24 * 30) {
        return std::nullopt;
    }
    return static_cast<int>(parsed);
}

std::size_t readSizeEnv(const std::string &key,
                        std::size_t fallback,
                        std::size_t maximum) {
    const auto value = readEnv(key);
    if (!value || value->empty()) {
        return fallback;
    }
    char *end = nullptr;
    const unsigned long parsed = std::strtoul(value->c_str(), &end, 10);
    if (end == value->c_str() || parsed == 0 || parsed > maximum) {
        return fallback;
    }
    return static_cast<std::size_t>(parsed);
}

bool persistentDbRequired() {
    return envFlagEnabled("CFF_REQUIRE_DB");
}

std::size_t minPasswordLength() {
    return readSizeEnv("CFF_MIN_PASSWORD_LENGTH", 12, 72);
}

std::size_t maxPasswordLength() {
    return readSizeEnv("CFF_MAX_PASSWORD_LENGTH", 72, 72);
}

bool sharedSecretAuthAllowed() {
    return envFlagEnabled("CFF_ALLOW_SHARED_SECRET_AUTH");
}

std::unordered_set<std::string> csvEmailSetFromEnv(const std::string &key) {
    std::unordered_set<std::string> values;
    const auto raw = readEnv(key);
    if (!raw) {
        return values;
    }
    std::size_t start = 0;
    while (start <= raw->size()) {
        const auto pos = raw->find(',', start);
        auto item = raw->substr(start, pos == std::string::npos ? std::string::npos : pos - start);
        item = trimAscii(std::move(item));
        if (!item.empty()) {
            values.insert(lowerAscii(std::move(item)));
        }
        if (pos == std::string::npos) {
            break;
        }
        start = pos + 1;
    }
    return values;
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

std::optional<std::string> frontendBaseUrl() {
    if (auto url = readEnv("CFF_FRONTEND_BASE_URL")) {
        if (!url->empty()) {
            return url;
        }
    }
    if (auto origins = readEnv("ALLOWED_ORIGINS")) {
        const auto comma = origins->find(',');
        auto first = origins->substr(0, comma == std::string::npos ? std::string::npos : comma);
        while (!first.empty() && first.back() == '/') {
            first.pop_back();
        }
        if (!first.empty()) {
            return first;
        }
    }
    return std::nullopt;
}

RuntimeConfig loadRuntimeConfig() {
    RuntimeConfig config;
    config.port = readEnv("PORT").value_or("8080");
    config.jwtSecret = readEnv("JWT_SECRET");
    config.sslCert = readEnv("SSL_CERT_FILE");
    config.sslKey = readEnv("SSL_KEY_FILE");

    const auto originEnv = readEnv("ALLOWED_ORIGINS");
    config.allowedOriginsConfigured = originEnv.has_value();
    if (originEnv) {
        config.allowedOrigins = splitOrigins(*originEnv);
    }

    config.ingestOnStartup = ingestOnStartupEnabled();
    config.ingestIntervalHours = readPositiveIntEnv("CFBD_INGEST_INTERVAL_HOURS");
    return config;
}

} // namespace cff::config
'''

app_config_tests = r'''#include "app_config.h"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

class ScopedEnvironment {
public:
    explicit ScopedEnvironment(std::vector<std::string> keys) : keys_(std::move(keys)) {
        for (const auto &key : keys_) {
            const char *value = std::getenv(key.c_str());
            previous_.push_back(value ? std::optional<std::string>{value} : std::nullopt);
            unsetenv(key.c_str());
        }
    }

    ~ScopedEnvironment() {
        for (std::size_t index = 0; index < keys_.size(); ++index) {
            if (previous_[index]) {
                setenv(keys_[index].c_str(), previous_[index]->c_str(), 1);
            } else {
                unsetenv(keys_[index].c_str());
            }
        }
    }

private:
    std::vector<std::string> keys_;
    std::vector<std::optional<std::string>> previous_;
};

int failures = 0;

void expect(bool condition, const std::string &message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void set(const char *key, const char *value) {
    if (setenv(key, value, 1) != 0) {
        std::perror("setenv");
        std::exit(2);
    }
}

} // namespace

int main() {
    ScopedEnvironment environment({
        "PORT", "JWT_SECRET", "SSL_CERT_FILE", "SSL_KEY_FILE",
        "ALLOWED_ORIGINS", "CFBD_INGEST_ON_STARTUP",
        "CFBD_INGEST_INTERVAL_HOURS", "CFF_REQUIRE_DB",
        "CFF_MIN_PASSWORD_LENGTH", "CFF_MAX_PASSWORD_LENGTH",
        "CFF_ALLOW_SHARED_SECRET_AUTH", "CFF_ADMIN_EMAILS",
        "CFF_REQUIRE_EMAIL_VERIFICATION", "CFF_EXPOSE_AUTH_TOKENS",
        "CFF_LOG_AUTH_TOKENS", "CFF_FRONTEND_BASE_URL", "TEST_SIZE"
    });

    using namespace cff::config;

    expect(!readEnv("PORT").has_value(), "unset environment values stay absent");
    set("PORT", "9090");
    expect(readEnv("PORT").value_or("") == "9090", "readEnv returns exact text");

    for (const auto *truthy : {"1", "true", "TRUE", "yes", "YES"}) {
        set("CFF_REQUIRE_DB", truthy);
        expect(envFlagEnabled("CFF_REQUIRE_DB"), std::string{"truthy flag accepted: "} + truthy);
    }
    for (const auto *falsey : {"0", "false", "True", "on", ""}) {
        set("CFF_REQUIRE_DB", falsey);
        expect(!envFlagEnabled("CFF_REQUIRE_DB"), std::string{"falsey flag rejected: "} + falsey);
    }

    set("CFBD_INGEST_INTERVAL_HOURS", "6");
    expect(readPositiveIntEnv("CFBD_INGEST_INTERVAL_HOURS") == 6, "positive interval parsed");
    set("CFBD_INGEST_INTERVAL_HOURS", "0");
    expect(!readPositiveIntEnv("CFBD_INGEST_INTERVAL_HOURS"), "zero interval rejected");
    set("CFBD_INGEST_INTERVAL_HOURS", "721");
    expect(!readPositiveIntEnv("CFBD_INGEST_INTERVAL_HOURS"), "interval maximum retained");

    set("TEST_SIZE", "24");
    expect(readSizeEnv("TEST_SIZE", 12, 72) == 24, "size value parsed");
    set("TEST_SIZE", "73");
    expect(readSizeEnv("TEST_SIZE", 12, 72) == 12, "oversized value uses fallback");
    set("TEST_SIZE", "12suffix");
    expect(readSizeEnv("TEST_SIZE", 8, 72) == 12, "legacy partial numeric parsing preserved");

    set("CFF_MIN_PASSWORD_LENGTH", "16");
    set("CFF_MAX_PASSWORD_LENGTH", "64");
    expect(minPasswordLength() == 16, "minimum password policy loaded");
    expect(maxPasswordLength() == 64, "maximum password policy loaded");

    set("CFF_ALLOW_SHARED_SECRET_AUTH", "true");
    set("CFF_REQUIRE_EMAIL_VERIFICATION", "YES");
    set("CFF_EXPOSE_AUTH_TOKENS", "1");
    set("CFF_LOG_AUTH_TOKENS", "yes");
    expect(sharedSecretAuthAllowed(), "shared secret policy loaded");
    expect(emailVerificationRequired(), "verification policy loaded");
    expect(exposeAuthTokens(), "token response policy loaded");
    expect(logAuthTokens(), "token logging policy loaded");

    set("CFF_ADMIN_EMAILS", " Admin@Example.COM,admin@example.com, second@example.com ");
    const auto admins = csvEmailSetFromEnv("CFF_ADMIN_EMAILS");
    expect(admins.size() == 2, "email list trims, lowercases, and deduplicates");
    expect(admins.count("admin@example.com") == 1, "canonical admin email present");
    expect(admins.count("second@example.com") == 1, "second admin email present");

    unsetenv("CFF_FRONTEND_BASE_URL");
    set("ALLOWED_ORIGINS", "https://frontend.example.test/,https://secondary.example.test");
    expect(frontendBaseUrl().value_or("") == "https://frontend.example.test",
           "frontend URL falls back to first allowed origin and removes trailing slash");
    set("CFF_FRONTEND_BASE_URL", "https://explicit.example.test/");
    expect(frontendBaseUrl().value_or("") == "https://explicit.example.test/",
           "explicit frontend URL behavior remains unchanged");

    set("PORT", "8181");
    set("JWT_SECRET", "test-secret");
    set("SSL_CERT_FILE", "/tmp/cert.pem");
    set("SSL_KEY_FILE", "/tmp/key.pem");
    set("ALLOWED_ORIGINS", "https://one.example.test,https://two.example.test");
    set("CFBD_INGEST_ON_STARTUP", "yes");
    set("CFBD_INGEST_INTERVAL_HOURS", "12");
    const auto runtime = loadRuntimeConfig();
    expect(runtime.port == "8181", "runtime port loaded");
    expect(runtime.jwtSecret == std::optional<std::string>{"test-secret"}, "runtime JWT secret loaded");
    expect(runtime.sslEnabled(), "SSL requires both certificate and key");
    expect(runtime.allowedOriginsConfigured, "origin presence tracked separately");
    expect(runtime.allowedOrigins.size() == 2, "runtime origins split without changing values");
    expect(runtime.ingestOnStartup, "legacy startup ingest flag behavior preserved");
    expect(runtime.ingestIntervalHours == 12, "runtime ingest interval loaded");

    unsetenv("SSL_KEY_FILE");
    expect(!loadRuntimeConfig().sslEnabled(), "partial SSL configuration stays disabled");
    set("CFBD_INGEST_ON_STARTUP", "YES");
    expect(!loadRuntimeConfig().ingestOnStartup, "legacy uppercase YES startup behavior preserved");

    if (failures != 0) {
        std::cerr << failures << " app configuration assertion(s) failed\n";
        return 1;
    }
    std::cout << "app configuration contracts passed\n";
    return 0;
}
'''

app_config_workflow = r'''name: App configuration tests

on:
  push:
    branches: [main, Test]
    paths:
      - "backend/src/app_config.h"
      - "backend/src/app_config.cpp"
      - "backend/src/main.cpp"
      - "backend/tests/app_config_tests.cpp"
      - "backend/CMakeLists.txt"
      - ".github/workflows/app-config-tests.yml"
  pull_request:
    branches: [main, Test]
    paths:
      - "backend/src/app_config.h"
      - "backend/src/app_config.cpp"
      - "backend/src/main.cpp"
      - "backend/tests/app_config_tests.cpp"
      - "backend/CMakeLists.txt"
      - ".github/workflows/app-config-tests.yml"
  workflow_dispatch:

permissions:
  contents: read

jobs:
  unit-contracts:
    name: Environment parsing and runtime configuration
    runs-on: ubuntu-24.04
    timeout-minutes: 10
    steps:
      - uses: actions/checkout@v4
      - name: Compile app configuration tests
        run: |
          g++ -std=c++17 -Wall -Wextra -Werror -pedantic \
            -Ibackend/src \
            backend/src/app_config.cpp \
            backend/tests/app_config_tests.cpp \
            -o /tmp/app_config_tests
      - name: Run app configuration tests
        run: /tmp/app_config_tests
'''

(ROOT / "backend/src/app_config.h").write_text(app_config_h, encoding="utf-8")
(ROOT / "backend/src/app_config.cpp").write_text(app_config_cpp, encoding="utf-8")
(ROOT / "backend/tests").mkdir(parents=True, exist_ok=True)
(ROOT / "backend/tests/app_config_tests.cpp").write_text(app_config_tests, encoding="utf-8")
(ROOT / ".github/workflows/app-config-tests.yml").write_text(app_config_workflow, encoding="utf-8")

cmake = CMAKE.read_text(encoding="utf-8")
cmake = replace_once(
    cmake,
    '''add_executable(college_ff_server
    src/main.cpp
    src/email_delivery.cpp''',
    '''add_executable(college_ff_server
    src/main.cpp
    src/app_config.cpp
    src/email_delivery.cpp''',
    "add app_config.cpp to server",
)
cmake += r'''

include(CTest)
if (BUILD_TESTING)
    add_executable(app_config_tests
        tests/app_config_tests.cpp
        src/app_config.cpp
    )
    target_include_directories(app_config_tests PRIVATE src)
    add_test(NAME app_config_tests COMMAND app_config_tests)
endif()
'''
CMAKE.write_text(cmake, encoding="utf-8")

print("app configuration extraction applied")
