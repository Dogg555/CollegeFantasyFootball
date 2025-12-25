#include "league_handler.h"

#ifdef DROGON_FOUND
#include <drogon/HttpResponse.h>
#include <json/json.h>
#include "../json_utils.h"
#include "../league_models.h"

namespace cff::handlers {

namespace {
Json::Value normalizeLeaguePayload(const Json::Value &body) {
    Json::Value normalized = body.isObject() ? body : Json::Value{Json::objectValue};
    // Apply defaults to reduce logic at the callsite.
    if (!normalized.isMember("name")) {
        normalized["name"] = "New League";
    }
    if (!normalized.isMember("teams")) {
        normalized["teams"] = 10;
    }
    if (!normalized.isMember("scoring")) {
        normalized["scoring"] = "ppr";
    }
    if (!normalized.isMember("draftType")) {
        normalized["draftType"] = "snake";
    }
    if (!normalized.isMember("notes")) {
        normalized["notes"] = "";
    }
    return normalized;
}

Json::Value buildLeagueResponse(const cff::League &league) {
    auto json = league.toJson();
    json["message"] = "League created";
    return json;
}
} // namespace

void handleCreateLeague(const drogon::HttpRequestPtr &req,
                        std::function<void (const drogon::HttpResponsePtr &)> &&callback) {
    const auto body = req->getJsonObject();
    const auto normalized = normalizeLeaguePayload(body ? *body : Json::Value{});
    const auto league = cff::League::fromJson(normalized);

    auto resp = drogon::HttpResponse::newHttpJsonResponse(buildLeagueResponse(league));
    resp->setStatusCode(drogon::k201Created);
    callback(resp);
}

} // namespace cff::handlers
#endif // DROGON_FOUND
