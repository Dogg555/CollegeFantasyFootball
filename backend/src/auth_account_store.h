#pragma once

#include <optional>
#include <string>

namespace cff::auth {

bool createInMemoryAccount(const std::string &email, const std::string &passwordHash);
std::optional<std::string> inMemoryPasswordHashForEmail(const std::string &email);

bool createPersistentAccount(const std::string &email, const std::string &passwordHash);
std::optional<std::string> persistentPasswordHashForEmail(const std::string &email);
std::optional<bool> persistentEmailVerified(const std::string &email);
bool storeEmailVerificationToken(const std::string &email, const std::string &token);
std::optional<std::string> verifyEmailToken(const std::string &token);
std::optional<std::string> storePasswordResetToken(const std::string &email,
                                                   const std::string &token);
std::optional<std::string> resetPassword(const std::string &token,
                                         const std::string &passwordHash);

} // namespace cff::auth
