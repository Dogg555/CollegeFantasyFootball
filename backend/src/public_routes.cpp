#include "public_routes.h"

#ifdef DROGON_FOUND
#include "http_security.h"
#include "live_scores.h"
#include "player_catalog.h"

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <string>

#include <drogon/drogon.h>
#include <json/json.h>

namespace {

std::optional<std::string> getOptionalParam(
    const drogon::HttpRequestPtr &request,
    const std::string &key
) {
    auto value = request->getParameter(key);
    if (value.empty()) {
        return std::nullopt;
    }
    return value;
}

} // namespace

namespace cff::public_api {

void registerPublicRoutes(
    drogon::HttpAppFramework &app,
    const std::unordered_set<std::string> &allowedOrigins
) {
    auto preflightHandler = [allowedOrigins](
        const drogon::HttpRequestPtr &request,
        std::function<void(const drogon::HttpResponsePtr &)> &&callback
    ) {
        callback(cff::http::buildPreflightResponse(request, allowedOrigins));
    };

    app.registerHandler(
           "/api/scores/live",
           [](const drogon::HttpRequestPtr &,
              std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
               auto response = drogon::HttpResponse::newHttpJsonResponse(
                   cff::cachedLiveScorePayload()
               );
               response->setStatusCode(drogon::k200OK);
               callback(response);
           },
           {drogon::Get}
       )
        .registerHandler(
            "/api/scores/live/meta",
            [](const drogon::HttpRequestPtr &,
               std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
                auto response = drogon::HttpResponse::newHttpJsonResponse(
                    cff::cachedLiveScoreMeta()
                );
                response->setStatusCode(drogon::k200OK);
                callback(response);
            },
            {drogon::Get}
        )
        .registerHandler(
            "/api/players/meta",
            [](const drogon::HttpRequestPtr &,
               std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
                auto response = drogon::HttpResponse::newHttpJsonResponse(
                    cff::playerCatalogMeta()
                );
                response->setStatusCode(drogon::k200OK);
                callback(response);
            },
            {drogon::Get}
        )
        .registerHandler(
            "/api/players",
            [](const drogon::HttpRequestPtr &request,
               std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
#ifndef CFF_HAS_POSTGRES
                Json::Value error;
                error["error"] =
                    "Player search unavailable: backend not built with PostgreSQL support.";
                auto response = drogon::HttpResponse::newHttpJsonResponse(error);
                response->setStatusCode(drogon::k503ServiceUnavailable);
                callback(response);
                return;
#else
                const auto query = request->getParameter("query");
                auto positionFilter = getOptionalParam(request, "position");
                auto conferenceFilter = getOptionalParam(request, "conference");
                auto teamFilter = getOptionalParam(request, "team");

                std::size_t limit = 25;
                const auto limitParam = request->getParameter("limit");
                if (!limitParam.empty()) {
                    char *end = nullptr;
                    const auto parsed = std::strtoul(limitParam.c_str(), &end, 10);
                    if (end != limitParam.c_str() && parsed > 0) {
                        limit = std::min<std::size_t>(parsed, 100);
                    }
                }

                std::size_t offset = 0;
                const auto offsetParam = request->getParameter("offset");
                if (!offsetParam.empty()) {
                    char *end = nullptr;
                    const auto parsed = std::strtoul(offsetParam.c_str(), &end, 10);
                    if (end != offsetParam.c_str()) {
                        offset = std::min<std::size_t>(parsed, 5000);
                    }
                }

                const auto results = cff::searchPlayers(
                    query,
                    positionFilter,
                    conferenceFilter,
                    teamFilter,
                    limit,
                    offset
                );
                Json::Value payload(Json::arrayValue);
                for (const auto &player : results) {
                    payload.append(player.toJson());
                }

                auto response = drogon::HttpResponse::newHttpJsonResponse(payload);
                response->setStatusCode(drogon::k200OK);
                callback(response);
#endif
            },
            {drogon::Get}
        )
        .registerHandler(
            "/api/scores/live",
            preflightHandler,
            {drogon::Options}
        )
        .registerHandler(
            "/api/scores/live/meta",
            preflightHandler,
            {drogon::Options}
        )
        .registerHandler(
            "/api/players",
            preflightHandler,
            {drogon::Options}
        )
        .registerHandler(
            "/api/players/meta",
            preflightHandler,
            {drogon::Options}
        );
}

} // namespace cff::public_api
#endif
