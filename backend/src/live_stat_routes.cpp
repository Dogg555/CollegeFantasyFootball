#include "live_stat_routes.h"

#include "http_security.h"
#include "live_stat_worker.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace cff::live_stats {
namespace {

bool parseInt(const std::string &value, int &output) {
    if (value.empty()) return false;
    try {
        std::size_t consumed = 0;
        const int parsed = std::stoi(value, &consumed);
        if (consumed != value.size()) return false;
        output = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

bool parseBool(const std::string &value, bool fallback = false) {
    if (value.empty()) return fallback;
    auto normalized = value;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (normalized == "true" || normalized == "1" || normalized == "yes") return true;
    if (normalized == "false" || normalized == "0" || normalized == "no") return false;
    return fallback;
}

WorkerRequest workerRequestFrom(const drogon::HttpRequestPtr &request) {
    WorkerRequest worker;
    worker.season = configuredLiveStatSeason();
    worker.week = configuredLiveStatWeek();

    int parsed = 0;
    if (parseInt(request->getParameter("season"), parsed)) worker.season = parsed;
    if (parseInt(request->getParameter("week"), parsed)) worker.week = parsed;
    worker.force = parseBool(request->getParameter("force"), false);
    worker.runKey = request->getParameter("runKey");

    const auto body = request->getJsonObject();
    if (!body || !body->isObject()) return worker;
    if (body->isMember("season") && (*body)["season"].isInt()) {
        worker.season = (*body)["season"].asInt();
    }
    if (body->isMember("week") && (*body)["week"].isInt()) {
        worker.week = (*body)["week"].asInt();
    }
    if (body->isMember("force") && (*body)["force"].isBool()) {
        worker.force = (*body)["force"].asBool();
    }
    if (body->isMember("runKey") && (*body)["runKey"].isString()) {
        worker.runKey = (*body)["runKey"].asString();
    }
    return worker;
}

void applyWorkerStatus(const Json::Value &payload,
                       const drogon::HttpResponsePtr &response) {
    const auto code = payload.get("code", "").asString();
    const auto status = payload.get("status", "").asString();
    if (code == "invalid_request" || status == "invalid") {
        response->setStatusCode(drogon::k400BadRequest);
    } else if (code == "ingest_already_running") {
        response->setStatusCode(drogon::k409Conflict);
    } else if (code == "claim_failed" || status == "unavailable") {
        response->setStatusCode(drogon::k503ServiceUnavailable);
    } else {
        response->setStatusCode(drogon::k200OK);
    }
}

} // namespace

void registerLiveStatRoutes(
    drogon::HttpAppFramework &app,
    const std::optional<std::string> &jwtSecret,
    const std::unordered_set<std::string> &allowedOrigins) {
    app.registerHandler(
        "/api/admin/live-stats/run",
        [jwtSecret](
            const drogon::HttpRequestPtr &request,
            std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
            std::string adminIdentity;
            if (!cff::http::requireAdmin(request, callback, jwtSecret, adminIdentity)) {
                return;
            }

            auto payload = runCfbdLiveStatWorker(workerRequestFrom(request));
            payload["requestedBy"] = adminIdentity;
            auto response = drogon::HttpResponse::newHttpJsonResponse(payload);
            applyWorkerStatus(payload, response);
            callback(response);
        },
        {drogon::Post});

    app.registerHandler(
        "/api/admin/live-stats/status",
        [jwtSecret](
            const drogon::HttpRequestPtr &request,
            std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
            std::string adminIdentity;
            if (!cff::http::requireAdmin(request, callback, jwtSecret, adminIdentity)) {
                return;
            }

            int season = 0;
            int week = -1;
            int parsed = 0;
            if (parseInt(request->getParameter("season"), parsed)) season = parsed;
            if (parseInt(request->getParameter("week"), parsed)) week = parsed;
            auto payload = liveStatOperatorStatus(season, week);
            payload["requestedBy"] = adminIdentity;
            auto response = drogon::HttpResponse::newHttpJsonResponse(payload);
            response->setStatusCode(
                payload.get("status", "unavailable").asString() == "ok"
                    ? drogon::k200OK
                    : drogon::k503ServiceUnavailable);
            callback(response);
        },
        {drogon::Get});

    const auto preflight = [allowedOrigins](
        const drogon::HttpRequestPtr &request,
        std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        callback(cff::http::buildPreflightResponse(request, allowedOrigins));
    };
    app.registerHandler(
        "/api/admin/live-stats/run", preflight, {drogon::Options});
    app.registerHandler(
        "/api/admin/live-stats/status", preflight, {drogon::Options});
}

} // namespace cff::live_stats
