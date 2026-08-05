#include "app_config.h"

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
        value = trimAscii(std::move(value));
        while (!value.empty() && value.back() == '/') {
            value.pop_back();
        }
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
