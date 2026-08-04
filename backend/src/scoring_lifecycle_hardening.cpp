#include <drogon/drogon.h>
#include <json/json.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <iomanip>
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
#include "league_schedule.h"
#include "scoring_lifecycle.h"

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
    return cff::scoring_lifecycle::canonicalEmail(std::move(value));
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

int currentSeasonYear() {
    const auto now = std::chrono::system_clock::now();
    const auto raw = std::chrono::system_clock::to_time_t(now);
    std::tm timeInfo{};
#ifdef _WIN32
    gmtime_s(&timeInfo, &raw);
#else
    gmtime_r(&raw, &timeInfo);
#endif
    return timeInfo.tm_year + 1900;
}

int positiveInt(const Json::Value &value, int fallback) {
    if (value.isInt() || value.isUInt()) return std::max(1, value.asInt());
    if (value.isString()) {
        try {
            return std::max(1, std::stoi(value.asString()));
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

int requestSeason(const drogon::HttpRequestPtr &request) {
    if (const auto body = request->getJsonObject(); body && body->isObject()) {
        if (body->isMember("season")) return positiveInt((*body)["season"], currentSeasonYear());
    }
    const auto query = request->getParameter("season");
    if (!query.empty()) {
        try {
            return std::max(1, std::stoi(query));
        } catch (...) {
        }
    }
    return currentSeasonYear();
}

int requestWeek(const drogon::HttpRequestPtr &request, int fallback = 1) {
    if (const auto body = request->getJsonObject(); body && body->isObject()) {
        if (body->isMember("week")) return positiveInt((*body)["week"], fallback);
    }
    const auto query = request->getParameter("week");
    if (!query.empty()) {
        try {
            return std::max(1, std::stoi(query));
        } catch (...) {
        }
    }
    return std::max(1, fallback);
}

std::string pathLeagueId(const std::string &path, const std::string &suffix) {
    const std::string prefix = "/api/leagues/";
    if (path.rfind(prefix, 0) != 0 || suffix.empty()) return "";
    if (path.size() <= prefix.size() + suffix.size()) return "";
    if (path.substr(path.size() - suffix.size()) != suffix) return "";
    return path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());
}

bool parseScoreWeekPath(const std::string &path,
                        const std::string &suffix,
                        std::string &leagueId,
                        int &week) {
    const std::string prefix = "/api/leagues/";
    const std::string marker = "/score/week/";
    if (path.rfind(prefix, 0) != 0) return false;
    if (!suffix.empty() && (path.size() <= suffix.size()
        || path.substr(path.size() - suffix.size()) != suffix)) return false;
    const auto markerAt = path.find(marker, prefix.size());
    if (markerAt == std::string::npos) return false;
    leagueId = path.substr(prefix.size(), markerAt - prefix.size());
    const auto weekStart = markerAt + marker.size();
    const auto weekLength = path.size() - weekStart - suffix.size();
    const auto rawWeek = path.substr(weekStart, weekLength);
    if (leagueId.empty() || rawWeek.empty() || rawWeek.find('/') != std::string::npos) return false;
    try {
        week = std::max(1, std::stoi(rawWeek));
    } catch (...) {
        return false;
    }
    return true;
}

#ifdef CFF_HAS_POSTGRES
#include "scoring_lifecycle_hardening_db.inc"
#include "scoring_lifecycle_hardening_payload.inc"
#include "scoring_lifecycle_hardening_mutations.inc"
#endif

#include "scoring_lifecycle_hardening_advice.inc"

} // namespace
