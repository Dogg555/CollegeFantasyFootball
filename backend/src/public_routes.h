#pragma once

#ifdef DROGON_FOUND
#include <drogon/HttpAppFramework.h>

#include <string>
#include <unordered_set>

namespace cff::public_api {

void registerPublicRoutes(
    drogon::HttpAppFramework &app,
    const std::unordered_set<std::string> &allowedOrigins
);

} // namespace cff::public_api
#endif
