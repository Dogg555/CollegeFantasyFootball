#pragma once

#include <drogon/drogon.h>

#include <optional>
#include <string>
#include <unordered_set>

namespace cff::commissioner_routes {

void registerCommissionerRoutes(
    drogon::HttpAppFramework &app,
    const std::optional<std::string> &jwtSecret,
    const std::unordered_set<std::string> &allowedOrigins);

} // namespace cff::commissioner_routes
