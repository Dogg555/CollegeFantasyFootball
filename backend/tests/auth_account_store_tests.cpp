#include "auth_account_store.h"

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
