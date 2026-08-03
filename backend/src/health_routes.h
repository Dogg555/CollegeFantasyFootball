#pragma once

#include <drogon/drogon.h>
#include <json/json.h>

#include <optional>
#include <string>
#include <unordered_set>

namespace cff::health {

Json::Value buildHealthPayload(
    const std::optional<std::string> &jwtSecret,
    const std::unordered_set<std::string> &allowedOrigins);

drogon::HttpStatusCode healthStatusCode(const Json::Value &payload);

void registerHealthRoutes(
    drogon::HttpAppFramework &app,
    const std::optional<std::string> &jwtSecret,
    const std::unordered_set<std::string> &allowedOrigins);

} // namespace cff::health
