#include "auth_core.h"

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
