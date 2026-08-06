#include <drogon/drogon.h>
#include <json/json.h>

#include <algorithm>
#include <cctype>
#include <string>

#include "app_config.h"
#include "http_security.h"

namespace {

std::string normalizeDraftType(std::string value) {
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), value.end());
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool supportedDraftType(const Json::Value *body) {
    if (!body || !body->isObject() || !body->isMember("draftType")) return true;
    if (!(*body)["draftType"].isString()) return false;
    const auto draftType = normalizeDraftType((*body)["draftType"].asString());
    return draftType.empty() || draftType == "snake";
}

drogon::HttpResponsePtr snakeDraftOnlyAdvice(const drogon::HttpRequestPtr &request) {
    if (request->getMethod() != drogon::Post || request->getPath() != "/api/leagues") {
        return nullptr;
    }

    const auto body = request->getJsonObject();
    if (supportedDraftType(body.get())) return nullptr;

    const auto runtimeConfig = cff::config::loadRuntimeConfig();
    if (!cff::http::accountEmailForRequest(request, runtimeConfig.jwtSecret)) {
        return nullptr;
    }

    Json::Value payload(Json::objectValue);
    payload["error"] = "Only snake drafts are available. Auction drafts are coming in a future release.";
    payload["code"] = "unsupported_draft_type";
    payload["retryable"] = false;
    auto response = drogon::HttpResponse::newHttpJsonResponse(payload);
    response->setStatusCode(drogon::k400BadRequest);
    return cff::http::withRuntimeCorsHeaders(request, response);
}

struct SnakeDraftOnlyInstaller {
    SnakeDraftOnlyInstaller() {
        drogon::app().registerSyncAdvice(snakeDraftOnlyAdvice);
    }
};

#if defined(__GNUC__) || defined(__clang__)
__attribute__((init_priority(200)))
#endif
SnakeDraftOnlyInstaller snakeDraftOnlyInstaller;

} // namespace
