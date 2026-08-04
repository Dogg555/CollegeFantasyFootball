#include <drogon/drogon.h>
#include <json/json.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef CFF_HAS_POSTGRES
#include <postgresql/libpq-fe.h>
#endif

#include "app_config.h"
#include "draft_lifecycle.h"
#include "http_security.h"
#include "league_roster.h"

namespace {

constexpr std::size_t kMaxOperationKeyLength = 128;
constexpr int kPresenceWindowSeconds = 30;

std::string trim(std::string value) {
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), value.end());
    return value;
}

std::string jsonToString(const Json::Value &value) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, value);
}

Json::Value jsonFromString(const std::string &raw,
                           Json::Value fallback = Json::Value{Json::objectValue}) {
    if (raw.empty()) return fallback;
    Json::CharReaderBuilder builder;
    Json::Value parsed;
    std::string errors;
    std::istringstream stream(raw);
    return Json::parseFromStream(builder, stream, &parsed, &errors) ? parsed : fallback;
}

Json::Value errorPayload(const std::string &message,
                         const std::string &code,
                         bool retryable = false) {
    Json::Value payload(Json::objectValue);
    payload["error"] = message;
    payload["code"] = code;
    payload["retryable"] = retryable;
    return payload;
}

drogon::HttpResponsePtr jsonResponse(const Json::Value &payload,
                                     drogon::HttpStatusCode status = drogon::k200OK) {
    auto response = drogon::HttpResponse::newHttpJsonResponse(payload);
    response->setStatusCode(status);
    return response;
}

drogon::HttpResponsePtr errorResponse(drogon::HttpStatusCode status,
                                      const std::string &message,
                                      const std::string &code,
                                      bool retryable = false,
                                      const Json::Value &details = Json::Value{Json::objectValue}) {
    auto payload = errorPayload(message, code, retryable);
    if (details.isObject()) {
        for (const auto &key : details.getMemberNames()) payload[key] = details[key];
    }
    return jsonResponse(payload, status);
}

std::string operationKey(const drogon::HttpRequestPtr &request) {
    auto value = trim(request->getHeader("Idempotency-Key"));
    if (value.empty()) value = trim(request->getHeader("X-Request-ID"));
    if (value.size() > kMaxOperationKeyLength) value.resize(kMaxOperationKeyLength);
    return value;
}

std::optional<std::string> accountEmail(const drogon::HttpRequestPtr &request) {
    const auto config = cff::config::loadRuntimeConfig();
    auto email = cff::http::accountEmailForRequest(request, config.jwtSecret);
    if (!email) return std::nullopt;
    return cff::draft_lifecycle::canonicalEmail(*email);
}

std::string pathLeagueId(const std::string &path, const std::string &suffix) {
    const std::string prefix = "/api/leagues/";
    if (path.rfind(prefix, 0) != 0 || suffix.empty()) return "";
    if (path.size() <= prefix.size() + suffix.size()) return "";
    if (path.substr(path.size() - suffix.size()) != suffix) return "";
    return path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());
}

#ifdef CFF_HAS_POSTGRES
#include "draft_lifecycle_hardening_db.inc"
#include "draft_lifecycle_hardening_payload.inc"
#include "draft_lifecycle_hardening_commissioner.inc"
#include "draft_lifecycle_hardening_pick.inc"
#include "draft_lifecycle_hardening_recovery.inc"
#endif

#include "draft_lifecycle_hardening_advice.inc"

} // namespace
