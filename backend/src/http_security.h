#pragma once

#include <drogon/drogon.h>

#include <functional>
#include <optional>
#include <string>
#include <unordered_set>

namespace cff::http {

std::optional<std::string> bearerToken(const drogon::HttpRequestPtr &req);

bool isAuthorized(const drogon::HttpRequestPtr &req,
                  const std::optional<std::string> &secret);

std::optional<std::string> accountEmailForRequest(
    const drogon::HttpRequestPtr &req,
    const std::optional<std::string> &secret);

bool requireAccount(
    const drogon::HttpRequestPtr &req,
    std::function<void(const drogon::HttpResponsePtr &)> &callback,
    const std::optional<std::string> &secret,
    std::string &accountEmail);

bool isAdminRequest(const drogon::HttpRequestPtr &req,
                    const std::optional<std::string> &secret,
                    std::string &adminIdentity);

bool requireAdmin(
    const drogon::HttpRequestPtr &req,
    std::function<void(const drogon::HttpResponsePtr &)> &callback,
    const std::optional<std::string> &secret,
    std::string &adminIdentity);

void applyCorsHeaders(
    const drogon::HttpRequestPtr &req,
    const drogon::HttpResponsePtr &resp,
    const std::unordered_set<std::string> &allowedOrigins);

drogon::HttpResponsePtr buildPreflightResponse(
    const drogon::HttpRequestPtr &req,
    const std::unordered_set<std::string> &allowedOrigins);

} // namespace cff::http
