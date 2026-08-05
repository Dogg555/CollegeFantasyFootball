#include <drogon/drogon.h>
#include <json/json.h>

#include <algorithm>
#include <chrono>
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
#include "http_security.h"
#include "league_roster.h"
#include "roster_transaction.h"

namespace {

constexpr std::size_t kMaxOperationKeyLength = 128;

std::string trim(std::string value) {
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), value.end());
    return value;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string canonicalEmail(std::string value) {
    return lower(trim(std::move(value)));
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
    return canonicalEmail(*email);
}

std::string pathLeagueId(const std::string &path, const std::string &suffix) {
    const std::string prefix = "/api/leagues/";
    if (path.rfind(prefix, 0) != 0 || suffix.empty()) return "";
    if (path.size() <= prefix.size() + suffix.size()) return "";
    if (path.substr(path.size() - suffix.size()) != suffix) return "";
    return path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());
}

bool parseRosterSlotPath(const std::string &path,
                         std::string &leagueId,
                         std::string &playerId) {
    const std::string prefix = "/api/leagues/";
    const std::string marker = "/roster/";
    const std::string suffix = "/slot";
    if (path.rfind(prefix, 0) != 0 || path.size() <= prefix.size() + marker.size() + suffix.size()) {
        return false;
    }
    if (path.substr(path.size() - suffix.size()) != suffix) return false;
    const auto markerAt = path.find(marker, prefix.size());
    if (markerAt == std::string::npos) return false;
    leagueId = path.substr(prefix.size(), markerAt - prefix.size());
    playerId = path.substr(markerAt + marker.size(),
                           path.size() - markerAt - marker.size() - suffix.size());
    return !leagueId.empty() && !playerId.empty() && playerId.find('/') == std::string::npos;
}

#ifdef CFF_HAS_POSTGRES
#include "roster_transaction_hardening_db.inc"
#include "roster_transaction_hardening_payload.inc"
#include "roster_transaction_hardening_mutations.inc"
#endif

#include "roster_transaction_hardening_advice.inc"

} // namespace
