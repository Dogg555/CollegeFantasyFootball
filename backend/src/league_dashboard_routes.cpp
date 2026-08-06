#include "league_dashboard_routes.h"

#include "http_security.h"
#include "league_dashboard.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <ctime>
#include <iomanip>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifdef CFF_HAS_POSTGRES
#include <postgresql/libpq-fe.h>
#endif

namespace cff::league_dashboard_routes {
namespace {

std::string canonical(std::string value) {
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

std::string utcNow() {
    const auto now = std::chrono::system_clock::now();
    const auto value = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &value);
#else
    gmtime_r(&value, &utc);
#endif
    std::ostringstream out;
    out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

drogon::HttpResponsePtr jsonResponse(
    const Json::Value &payload,
    drogon::HttpStatusCode status = drogon::k200OK) {
    auto response = drogon::HttpResponse::newHttpJsonResponse(payload);
    response->setStatusCode(status);
    response->addHeader("Cache-Control", "no-store");
    return response;
}

void sendError(
    std::function<void(const drogon::HttpResponsePtr &)> &callback,
    drogon::HttpStatusCode status,
    const std::string &code,
    const std::string &message) {
    Json::Value payload(Json::objectValue);
    payload["error"] = message;
    payload["code"] = code;
    callback(jsonResponse(payload, status));
}

Json::Value parseJson(const std::string &raw, Json::Value fallback = Json::Value(Json::objectValue)) {
    if (raw.empty()) return fallback;
    Json::Value value;
    Json::CharReaderBuilder builder;
    std::string errors;
    std::istringstream stream(raw);
    if (!Json::parseFromStream(builder, stream, &value, &errors)) return fallback;
    return value;
}

#ifdef CFF_HAS_POSTGRES
#include "league_dashboard_routes_db.inc"
#include "league_dashboard_routes_state.inc"
#include "league_dashboard_routes_actions.inc"
#include "league_dashboard_routes_handler.inc"
#endif

} // namespace

void registerLeagueDashboardRoutes(
    drogon::HttpAppFramework &app,
    const std::optional<std::string> &jwtSecret,
    const std::unordered_set<std::string> &allowedOrigins) {
    app.registerHandler(
        "/api/leagues/{1}/dashboard",
        [jwtSecret](const drogon::HttpRequestPtr &request,
                    std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                    const std::string &leagueId) {
#ifdef CFF_HAS_POSTGRES
            handleDashboard(request, std::move(callback), jwtSecret, leagueId);
#else
            (void)request;
            (void)jwtSecret;
            (void)leagueId;
            sendError(callback, drogon::k503ServiceUnavailable, "dashboard_unavailable",
                      "League dashboard requires PostgreSQL.");
#endif
        },
        {drogon::Get});

    app.registerHandler(
        "/api/leagues/{1}/dashboard",
        [allowedOrigins](const drogon::HttpRequestPtr &request,
                         std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                         const std::string &) {
            callback(cff::http::buildPreflightResponse(request, allowedOrigins));
        },
        {drogon::Options});
}

} // namespace cff::league_dashboard_routes
