#pragma once

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
