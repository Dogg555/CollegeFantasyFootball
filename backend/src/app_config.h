#pragma once

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
