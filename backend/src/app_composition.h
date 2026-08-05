#pragma once

#include <drogon/drogon.h>

#include <optional>
#include <string>
#include <unordered_set>

namespace cff::app_composition {

void registerApplicationRoutes(
    drogon::HttpAppFramework &app,
    const std::optional<std::string> &jwtSecret,
    const std::unordered_set<std::string> &allowedOrigins);

} // namespace cff::app_composition
