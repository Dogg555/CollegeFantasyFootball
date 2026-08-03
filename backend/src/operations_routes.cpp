#include "operations_routes.h"

#include "app_config.h"
#include "cfbd_ingest.h"
#include "http_security.h"
#include "live_scores.h"

#include <iostream>
#include <memory>
#include <utility>
#include <vector>

#ifdef CFF_HAS_POSTGRES
#include <postgresql/libpq-fe.h>
#endif

namespace cff::operations {
namespace {

#ifdef CFF_HAS_POSTGRES
struct PgConnDeleter {
    void operator()(PGconn *conn) const {
        if (conn) {
            PQfinish(conn);
        }
    }
};

struct PgResultDeleter {
    void operator()(PGresult *result) const {
        if (result) {
            PQclear(result);
        }
    }
};

using PgConnPtr = std::unique_ptr<PGconn, PgConnDeleter>;
using PgResultPtr = std::unique_ptr<PGresult, PgResultDeleter>;

bool databaseConfigured() {
    const auto url = cff::config::readEnv("DB_URL");
    return url && !url->empty();
}

PgConnPtr connectToDatabase() {
    const auto url = cff::config::readEnv("DB_URL");
    if (!url || url->empty()) {
        return nullptr;
    }

    auto conn = PgConnPtr{PQconnectdb(url->c_str())};
    if (PQstatus(conn.get()) != CONNECTION_OK) {
        std::cerr << "[auth] Failed to connect to Postgres: "
                  << PQerrorMessage(conn.get()) << std::endl;
        return nullptr;
    }
    return conn;
}

PgResultPtr executeParameters(
    PGconn *conn,
    const std::string &sql,
    const std::vector<std::string> &params) {
    std::vector<const char *> values;
    values.reserve(params.size());
    for (const auto &param : params) {
        values.push_back(param.c_str());
    }
    return PgResultPtr{PQexecParams(
        conn,
        sql.c_str(),
        static_cast<int>(values.size()),
        nullptr,
        values.data(),
        nullptr,
        nullptr,
        0)};
}

bool resultOk(PGresult *result, ExecStatusType expected) {
    return result && PQresultStatus(result) == expected;
}

Json::Value ingestionStatusPayload() {
    Json::Value payload;
    payload["configured"] = databaseConfigured();
    payload["cfbdApiConfigured"] =
        cff::config::readEnv("CFBD_API_KEY").has_value();
    payload["season"] = cff::config::readEnv("CFBD_SEASON").value_or("");
    payload["fullRosterSchedule"] = "weekly";
    payload["manualTriggerAvailable"] = true;
    payload["ready"] = false;
    payload["runs"] = Json::Value{Json::arrayValue};
    payload["counts"] = Json::Value{Json::objectValue};

    auto conn = connectToDatabase();
    if (!conn) {
        payload["status"] = "unavailable";
        payload["error"] = "Postgres is unavailable.";
        return payload;
    }

    auto counts = executeParameters(
        conn.get(),
        "SELECT "
        "(SELECT COUNT(*) FROM teams), "
        "(SELECT COUNT(*) FROM players), "
        "(SELECT COUNT(*) FROM games), "
        "(SELECT COUNT(*) FROM player_stats)",
        {});
    if (resultOk(counts.get(), PGRES_TUPLES_OK)
        && PQntuples(counts.get()) > 0) {
        payload["counts"]["teams"] = static_cast<Json::Int64>(
            std::stoll(PQgetvalue(counts.get(), 0, 0)));
        payload["counts"]["players"] = static_cast<Json::Int64>(
            std::stoll(PQgetvalue(counts.get(), 0, 1)));
        payload["counts"]["games"] = static_cast<Json::Int64>(
            std::stoll(PQgetvalue(counts.get(), 0, 2)));
        payload["counts"]["playerStats"] = static_cast<Json::Int64>(
            std::stoll(PQgetvalue(counts.get(), 0, 3)));
        payload["ready"] = payload["counts"]["players"].asInt64() > 0;
    }

    auto latest = executeParameters(
        conn.get(),
        "SELECT resource, COALESCE(status, ''), "
        "COALESCE(to_char(finished_at AT TIME ZONE 'UTC', "
        "'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), ''), "
        "COALESCE(error_message, '') "
        "FROM ingestion_runs WHERE resource IN ('players', 'scoreboard') "
        "ORDER BY started_at DESC LIMIT 1",
        {});
    if (resultOk(latest.get(), PGRES_TUPLES_OK)
        && PQntuples(latest.get()) > 0) {
        payload["latestRun"]["resource"] = PQgetvalue(latest.get(), 0, 0);
        payload["latestRun"]["status"] = PQgetvalue(latest.get(), 0, 1);
        payload["latestRun"]["finishedAt"] = PQgetvalue(latest.get(), 0, 2);
        payload["latestRun"]["error"] = PQgetvalue(latest.get(), 0, 3);
    }

    auto runs = executeParameters(
        conn.get(),
        "SELECT id, resource, COALESCE(season, 0), COALESCE(week, 0), "
        "COALESCE(to_char(started_at AT TIME ZONE 'UTC', "
        "'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), ''), "
        "COALESCE(to_char(finished_at AT TIME ZONE 'UTC', "
        "'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), ''), "
        "COALESCE(status, ''), call_count, row_count, "
        "COALESCE(error_message, '') "
        "FROM ingestion_runs ORDER BY started_at DESC LIMIT 10",
        {});
    if (resultOk(runs.get(), PGRES_TUPLES_OK)) {
        for (int row = 0; row < PQntuples(runs.get()); ++row) {
            Json::Value run;
            run["id"] = static_cast<Json::Int64>(
                std::stoll(PQgetvalue(runs.get(), row, 0)));
            run["resource"] = PQgetvalue(runs.get(), row, 1);
            run["season"] = std::stoi(PQgetvalue(runs.get(), row, 2));
            run["week"] = std::stoi(PQgetvalue(runs.get(), row, 3));
            run["startedAt"] = PQgetvalue(runs.get(), row, 4);
            run["finishedAt"] = PQgetvalue(runs.get(), row, 5);
            run["status"] = PQgetvalue(runs.get(), row, 6);
            run["apiCalls"] = std::stoi(PQgetvalue(runs.get(), row, 7));
            run["rowCount"] = std::stoi(PQgetvalue(runs.get(), row, 8));
            run["error"] = PQgetvalue(runs.get(), row, 9);
            payload["runs"].append(run);
        }
    }

    payload["status"] = "ok";
    return payload;
}
#endif

void appendErrors(Json::Value &payload, const std::vector<std::string> &errors) {
    if (errors.empty()) {
        return;
    }
    Json::Value values(Json::arrayValue);
    for (const auto &error : errors) {
        values.append(error);
    }
    payload["errors"] = values;
}

} // namespace

void registerOperationsRoutes(
    drogon::HttpAppFramework &app,
    const std::optional<std::string> &jwtSecret,
    const std::unordered_set<std::string> &allowedOrigins) {
    app.registerHandler(
        "/api/secure/ping",
        [jwtSecret](
            const drogon::HttpRequestPtr &request,
            std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
            auto response = drogon::HttpResponse::newHttpResponse();
            if (!cff::http::isAuthorized(request, jwtSecret)) {
                response->setStatusCode(drogon::k401Unauthorized);
                response->setBody("unauthorized");
                callback(response);
                return;
            }
            response->setStatusCode(drogon::k200OK);
            response->setBody(R"({"status":"ok","scope":"secure"})");
            response->addHeader("Content-Type", "application/json");
            callback(response);
        },
        {drogon::Post, drogon::Get});

    app.registerHandler(
        "/api/admin/ingest/cfbd",
        [jwtSecret](
            const drogon::HttpRequestPtr &request,
            std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
            std::string adminIdentity;
            if (!cff::http::requireAdmin(
                    request, callback, jwtSecret, adminIdentity)) {
                return;
            }

            const auto ingestResult = cff::runCfbdIngestOnce();
            Json::Value payload;
            payload["status"] =
                ingestResult.errors.empty() ? "ok" : "partial";
            payload["ingested"] =
                static_cast<Json::UInt64>(ingestResult.ingested);
            payload["updated"] =
                static_cast<Json::UInt64>(ingestResult.updated);
            payload["apiCalls"] =
                static_cast<Json::UInt64>(ingestResult.apiCalls);
            appendErrors(payload, ingestResult.errors);

            auto response = drogon::HttpResponse::newHttpJsonResponse(payload);
            response->setStatusCode(drogon::k200OK);
            callback(response);
        },
        {drogon::Post});

    app.registerHandler(
        "/api/admin/ingest/cfbd/status",
        [jwtSecret](
            const drogon::HttpRequestPtr &request,
            std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
            std::string adminIdentity;
            if (!cff::http::requireAdmin(
                    request, callback, jwtSecret, adminIdentity)) {
                return;
            }
#ifndef CFF_HAS_POSTGRES
            Json::Value payload;
            payload["configured"] = false;
            payload["status"] = "unavailable";
            payload["error"] =
                "Backend was not built with PostgreSQL support.";
            auto response = drogon::HttpResponse::newHttpJsonResponse(payload);
            response->setStatusCode(drogon::k503ServiceUnavailable);
            callback(response);
#else
            auto response = drogon::HttpResponse::newHttpJsonResponse(
                ingestionStatusPayload());
            response->setStatusCode(drogon::k200OK);
            callback(response);
#endif
        },
        {drogon::Get});

    app.registerHandler(
        "/api/admin/ingest/cfbd/live",
        [jwtSecret](
            const drogon::HttpRequestPtr &request,
            std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
            std::string adminIdentity;
            if (!cff::http::requireAdmin(
                    request, callback, jwtSecret, adminIdentity)) {
                return;
            }

            const auto ingestResult = cff::runLiveScoreIngestOnce();
            Json::Value payload;
            payload["status"] =
                ingestResult.errors.empty() ? "ok" : "partial";
            payload["games"] =
                static_cast<Json::UInt64>(ingestResult.games);
            payload["liveGames"] =
                static_cast<Json::UInt64>(ingestResult.liveGames);
            payload["apiCalls"] =
                static_cast<Json::UInt64>(ingestResult.apiCalls);
            appendErrors(payload, ingestResult.errors);

            auto response = drogon::HttpResponse::newHttpJsonResponse(payload);
            response->setStatusCode(drogon::k200OK);
            callback(response);
        },
        {drogon::Post});

    app.registerHandler(
        "/api/admin/ingest/cfbd/live/status",
        [jwtSecret](
            const drogon::HttpRequestPtr &request,
            std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
            std::string adminIdentity;
            if (!cff::http::requireAdmin(
                    request, callback, jwtSecret, adminIdentity)) {
                return;
            }
            auto response = drogon::HttpResponse::newHttpJsonResponse(
                cff::liveScoreIngestStatus());
            response->setStatusCode(drogon::k200OK);
            callback(response);
        },
        {drogon::Get});

    const auto preflight = [allowedOrigins](
        const drogon::HttpRequestPtr &request,
        std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        callback(cff::http::buildPreflightResponse(request, allowedOrigins));
    };

    app.registerHandler(
        "/api/secure/ping", preflight, {drogon::Options});
    app.registerHandler(
        "/api/admin/ingest/cfbd", preflight, {drogon::Options});
    app.registerHandler(
        "/api/admin/ingest/cfbd/status", preflight, {drogon::Options});
    app.registerHandler(
        "/api/admin/ingest/cfbd/live", preflight, {drogon::Options});
    app.registerHandler(
        "/api/admin/ingest/cfbd/live/status", preflight, {drogon::Options});
}

} // namespace cff::operations
