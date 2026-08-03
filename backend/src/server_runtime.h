#pragma once

#include "app_config.h"

#include <drogon/drogon.h>

#include <string>

namespace cff::server_runtime {

bool configureSecurityAndCors(
    drogon::HttpAppFramework &app,
    const cff::config::RuntimeConfig &runtimeConfig);

void configureListener(
    drogon::HttpAppFramework &app,
    const std::string &port,
    bool useSsl);

} // namespace cff::server_runtime
