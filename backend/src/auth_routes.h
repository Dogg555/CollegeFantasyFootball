#pragma once

#include <drogon/drogon.h>

#include <optional>
#include <string>

namespace cff::auth {

void registerAuthRoutes(drogon::HttpAppFramework &app,
                        const std::optional<std::string> &jwtSecret);

} // namespace cff::auth
