#pragma once

#include <optional>
#include <string>

namespace cff::auth {

std::optional<std::string> issueSessionToken(const std::string &email);
std::optional<std::string> emailForSessionToken(const std::string &token);
bool revokeSessionToken(const std::string &token);

} // namespace cff::auth
