#pragma once

#include "handlers/league_handler.h"
#include "http_security.h"
#include "league_context.h"

#include <drogon/drogon.h>

#include <chrono>
#include <ctime>
#include <functional>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>

namespace cff::league_context {
namespace detail {

inline std::string serverIsoNow() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t timestamp = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &timestamp);
#else
    gmtime_r(&timestamp, &utc);
#endif
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

inline drogon::HttpResponsePtr invalidLeaguePayload() {
    Json::Value payload(Json::objectValue);
    payload["error"] = "The league context response was invalid.";
    payload["code"] = "league_context_invalid";
    auto response = drogon::HttpResponse::newHttpJsonResponse(payload);
    response->setStatusCode(static_cast<drogon::HttpStatusCode>(502));
    response->addHeader("Cache-Control", "no-store");
    return response;
}

}  // namespace detail

inline void registerLeagueContextRoutes(
    drogon::HttpAppFramework &app,
    const std::optional<std::string> &jwtSecret,
    const std::unordered_set<std::string> &allowedOrigins) {
    (void)allowedOrigins;
    app.registerHandler(
        "/api/leagues/{1}/context",
        [jwtSecret](const drogon::HttpRequestPtr &req,
                    std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                    const std::string &leagueId) {
            std::string accountEmail;
            if (!cff::http::requireAccount(req, callback, jwtSecret, accountEmail)) {
                return;
            }

            std::function<void(const drogon::HttpResponsePtr &)> contextCallback =
                [callback = std::move(callback), accountEmail](const drogon::HttpResponsePtr &response) mutable {
                    if (!response || response->getStatusCode() != drogon::k200OK) {
                        callback(response ? response : detail::invalidLeaguePayload());
                        return;
                    }
                    const auto league = response->getJsonObject();
                    if (!league || !league->isObject() || league->get("id", "").asString().empty()) {
                        callback(detail::invalidLeaguePayload());
                        return;
                    }

                    auto context = buildLeagueContext(*league, accountEmail, false, detail::serverIsoNow());
                    auto contextResponse = drogon::HttpResponse::newHttpJsonResponse(context);
                    contextResponse->setStatusCode(drogon::k200OK);
                    contextResponse->addHeader("Cache-Control", "no-store");
                    callback(contextResponse);
                };

            // Reuse the existing league read path so membership checks and the
            // not-found response remain identical to other league-scoped reads.
            cff::handlers::handleGetLeague(
                req,
                std::move(contextCallback),
                accountEmail,
                leagueId);
        },
        {drogon::Get});
}

}  // namespace cff::league_context
