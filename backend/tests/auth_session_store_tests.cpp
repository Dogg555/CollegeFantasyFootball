#include "auth_session_store.h"

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
