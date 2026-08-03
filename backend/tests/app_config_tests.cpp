#include "app_config.h"

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
