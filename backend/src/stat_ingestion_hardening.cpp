#include <drogon/drogon.h>
#include <json/json.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifdef CFF_HAS_POSTGRES
#include <postgresql/libpq-fe.h>
#endif

#include "app_config.h"
#include "http_security.h"
#include "stat_ingestion_lifecycle.h"

namespace {

constexpr std::size_t kMaxOperationKeyLength = 128;
constexpr int kDefaultLeaseSeconds = 300;
constexpr int kDefaultStaleAfterSeconds = 900;

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

std::string operationKey(const drogon::HttpRequestPtr &request) {
    auto value = trim(request->getHeader("Idempotency-Key"));
    if (value.empty()) value = trim(request->getHeader("X-Request-ID"));
    if (value.size() > kMaxOperationKeyLength) value.resize(kMaxOperationKeyLength);
    return value;
}

int positiveInt(const Json::Value &value, int fallback) {
    if (value.isInt() || value.isUInt()) return std::max(1, value.asInt());
    if (value.isString()) {
        try { return std::max(1, std::stoi(value.asString())); } catch (...) { return fallback; }
    }
    return fallback;
}

long long int64Value(const Json::Value &value, long long fallback = -1) {
    if (value.isInt64() || value.isUInt64() || value.isInt() || value.isUInt()) return value.asInt64();
    if (value.isString()) {
        try { return std::stoll(value.asString()); } catch (...) { return fallback; }
    }
    return fallback;
}

double numberValue(const Json::Value &value, double fallback = 0.0) {
    if (value.isNumeric()) return value.asDouble();
    if (value.isString()) {
        try { return std::stod(value.asString()); } catch (...) { return fallback; }
    }
    return fallback;
}

int requestSeason(const drogon::HttpRequestPtr &request) {
    if (const auto body = request->getJsonObject(); body && body->isObject() && body->isMember("season")) {
        return positiveInt((*body)["season"], currentSeasonYear());
    }
    const auto query = request->getParameter("season");
    if (!query.empty()) {
        try { return std::max(1, std::stoi(query)); } catch (...) {}
    }
    return currentSeasonYear();
}

int requestWeek(const drogon::HttpRequestPtr &request) {
    if (const auto body = request->getJsonObject(); body && body->isObject() && body->isMember("week")) {
        return positiveInt((*body)["week"], 1);
    }
    const auto query = request->getParameter("week");
    if (!query.empty()) {
        try { return std::max(1, std::stoi(query)); } catch (...) {}
    }
    return 1;
}

#ifdef CFF_HAS_POSTGRES
#include "stat_ingestion_hardening_db.inc"
#include "stat_ingestion_hardening_payload.inc"
#include "stat_ingestion_hardening_mutations.inc"
#endif

#include "stat_ingestion_hardening_advice.inc"

} // namespace
