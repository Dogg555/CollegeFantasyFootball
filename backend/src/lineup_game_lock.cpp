#include <drogon/drogon.h>
#include <json/json.h>
#include <postgresql/libpq-fe.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "app_config.h"
#include "http_security.h"

namespace {

struct PgConnectionDeleter {
    void operator()(PGconn *connection) const {
        if (connection) PQfinish(connection);
    }
};

struct PgResultDeleter {
    void operator()(PGresult *result) const {
        if (result) PQclear(result);
    }
};

using PgConnection = std::unique_ptr<PGconn, PgConnectionDeleter>;
using PgResult = std::unique_ptr<PGresult, PgResultDeleter>;
using Callback = std::function<void(const drogon::HttpResponsePtr &)>;

struct WeekContext {
    int season{0};
    int week{1};
};

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

int currentSeasonYear() {
    const auto now = std::time(nullptr);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    return utc.tm_year + 1900;
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

PgConnection connectDb() {
    const auto *url = std::getenv("DB_URL");
    if (!url || !*url) return nullptr;
    PgConnection connection{PQconnectdb(url)};
    if (!connection || PQstatus(connection.get()) != CONNECTION_OK) return nullptr;
    return connection;
}

PgResult execute(PGconn *connection,
                 const std::string &sql,
                 const std::vector<std::string> &parameters = {}) {
    std::vector<const char *> values;
    values.reserve(parameters.size());
    for (const auto &parameter : parameters) values.push_back(parameter.c_str());
    return PgResult{PQexecParams(connection,
                                 sql.c_str(),
                                 static_cast<int>(values.size()),
                                 nullptr,
                                 values.empty() ? nullptr : values.data(),
                                 nullptr,
                                 nullptr,
                                 0)};
}

bool tuplesOk(const PgResult &result) {
    return result && PQresultStatus(result.get()) == PGRES_TUPLES_OK;
}

std::string cell(PGresult *result, int row, int column) {
    return PQgetisnull(result, row, column) ? "" : PQgetvalue(result, row, column);
}

bool truthy(PGresult *result, int row, int column) {
    return cell(result, row, column) == "t";
}

std::optional<std::string> accountEmail(const drogon::HttpRequestPtr &request) {
    const auto config = cff::config::loadRuntimeConfig();
    auto email = cff::http::accountEmailForRequest(request, config.jwtSecret);
    if (!email) return std::nullopt;
    return lower(trim(*email));
}

std::string pathLeagueId(const std::string &path, const std::string &suffix) {
    const std::string prefix = "/api/leagues/";
    if (path.rfind(prefix, 0) != 0 || suffix.empty()) return "";
    if (path.size() <= prefix.size() + suffix.size()) return "";
    if (path.substr(path.size() - suffix.size()) != suffix) return "";
    const auto leagueId = path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());
    return leagueId.find('/') == std::string::npos ? leagueId : "";
}

bool canAccessLeague(PGconn *connection,
                     const std::string &leagueId,
                     const std::string &email) {
    auto result = execute(connection,
        "SELECT EXISTS (SELECT 1 FROM leagues league WHERE league.id = $1 AND ("
        "lower(league.account_email) = lower($2) OR EXISTS (SELECT 1 FROM league_members member "
        "WHERE member.league_id = league.id AND lower(member.email) = lower($2) "
        "AND member.status = 'active'))) ",
        {leagueId, email});
    return tuplesOk(result) && PQntuples(result.get()) > 0 && truthy(result.get(), 0, 0);
}

WeekContext activeWeek(PGconn *connection, const std::string &leagueId) {
    WeekContext context{currentSeasonYear(), 1};
    auto result = execute(connection,
        "SELECT week_state.season, week_state.week FROM schedule_week_states week_state "
        "LEFT JOIN scoring_week_states scoring ON scoring.league_id = week_state.league_id "
        "AND scoring.season = week_state.season AND scoring.week = week_state.week "
        "WHERE week_state.league_id = $1 AND COALESCE(scoring.status, 'unscored') <> 'final' "
        "ORDER BY week_state.season DESC, week_state.week ASC LIMIT 1",
        {leagueId});
    if (!tuplesOk(result) || PQntuples(result.get()) == 0) return context;
    try {
        context.season = std::max(1, std::stoi(cell(result.get(), 0, 0)));
        context.week = std::max(1, std::stoi(cell(result.get(), 0, 1)));
    } catch (...) {
        return WeekContext{currentSeasonYear(), 1};
    }
    return context;
}

WeekContext requestWeek(const drogon::HttpRequestPtr &request,
                        PGconn *connection,
                        const std::string &leagueId) {
    auto context = activeWeek(connection, leagueId);
    if (const auto body = request->getJsonObject(); body && body->isObject()) {
        context.season = positiveInt(body->get("season", context.season), context.season);
        context.week = positiveInt(body->get("week", context.week), context.week);
    }
    const auto season = request->getParameter("season");
    const auto week = request->getParameter("week");
    if (!season.empty()) {
        try { context.season = std::max(1, std::stoi(season)); } catch (...) {}
    }
    if (!week.empty()) {
        try { context.week = std::max(1, std::stoi(week)); } catch (...) {}
    }
    return context;
}

bool weekLocked(PGconn *connection,
                const std::string &leagueId,
                const std::string &email,
                const WeekContext &context) {
    auto result = execute(connection,
        "SELECT EXISTS ("
        "SELECT 1 FROM lineup_week_states lineup WHERE lineup.league_id = $1 "
        "AND lower(lineup.manager_email) = lower($2) AND lineup.season = $3::int "
        "AND lineup.week = $4::int AND lineup.status IN ('locked', 'finalized') "
        "UNION ALL SELECT 1 FROM schedule_week_states week_state WHERE week_state.league_id = $1 "
        "AND week_state.season = $3::int AND week_state.week = $4::int "
        "AND week_state.status IN ('locked', 'finalized'))",
        {leagueId, email, std::to_string(context.season), std::to_string(context.week)});
    return tuplesOk(result) && PQntuples(result.get()) > 0 && truthy(result.get(), 0, 0);
}

Json::Value playerLocks(PGconn *connection,
                        const std::string &leagueId,
                        const std::string &email,
                        const WeekContext &context) {
    auto result = execute(connection,
        "WITH roster_players AS ("
        "SELECT roster.player_id, COALESCE(NULLIF(roster.player_snapshot->>'team', ''), "
        "NULLIF(roster.player_snapshot->>'school', ''), NULLIF(player.team, ''), '') AS team "
        "FROM rosters roster LEFT JOIN players player ON player.id = roster.player_id "
        "WHERE roster.league_id = $1 AND lower(roster.manager_email) = lower($2)) "
        "SELECT roster_players.player_id, roster_players.team, COALESCE(game.id::text, ''), "
        "COALESCE(to_char(game.start_date AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), ''), "
        "COALESCE(game.start_date <= NOW(), FALSE) "
        "FROM roster_players LEFT JOIN LATERAL (SELECT scheduled.id, scheduled.start_date FROM games scheduled "
        "WHERE scheduled.season = $3::int AND scheduled.week = $4::int "
        "AND (lower(scheduled.home_team) = lower(roster_players.team) "
        "OR lower(scheduled.away_team) = lower(roster_players.team)) "
        "ORDER BY scheduled.start_date NULLS LAST, scheduled.id LIMIT 1) game ON TRUE "
        "ORDER BY roster_players.player_id",
        {leagueId, email, std::to_string(context.season), std::to_string(context.week)});

    Json::Value locks(Json::arrayValue);
    if (!tuplesOk(result)) return Json::Value{};
    for (int row = 0; row < PQntuples(result.get()); ++row) {
        Json::Value lock(Json::objectValue);
        lock["playerId"] = cell(result.get(), row, 0);
        lock["team"] = cell(result.get(), row, 1);
        lock["gameId"] = cell(result.get(), row, 2);
        lock["gameStartTime"] = cell(result.get(), row, 3);
        lock["locked"] = truthy(result.get(), row, 4);
        lock["season"] = context.season;
        lock["week"] = context.week;
        locks.append(lock);
    }
    return locks;
}

Json::Value lockState(PGconn *connection,
                      const std::string &leagueId,
                      const std::string &email,
                      const WeekContext &context) {
    Json::Value payload(Json::objectValue);
    payload["leagueId"] = leagueId;
    payload["managerEmail"] = email;
    payload["season"] = context.season;
    payload["week"] = context.week;
    payload["weekLocked"] = weekLocked(connection, leagueId, email, context);
    payload["players"] = playerLocks(connection, leagueId, email, context);
    payload["serverTime"] = drogon::utils::getHttpFullDate(std::chrono::system_clock::now());
    return payload;
}

drogon::HttpResponsePtr jsonResponse(const Json::Value &payload,
                                     drogon::HttpStatusCode status = drogon::k200OK) {
    auto response = drogon::HttpResponse::newHttpJsonResponse(payload);
    response->setStatusCode(status);
    return response;
}

drogon::HttpResponsePtr errorResponse(const std::string &message,
                                      const std::string &code,
                                      drogon::HttpStatusCode status,
                                      bool retryable = false,
                                      const Json::Value &details = Json::Value{Json::objectValue}) {
    Json::Value payload(Json::objectValue);
    payload["error"] = message;
    payload["code"] = code;
    payload["retryable"] = retryable;
    if (details.isObject()) {
        for (const auto &key : details.getMemberNames()) payload[key] = details[key];
    }
    return jsonResponse(payload, status);
}

void handleLineupLocks(const drogon::HttpRequestPtr &request,
                       Callback &&callback,
                       const std::string &leagueId) {
    const auto respond = [&request, &callback](const drogon::HttpResponsePtr &response) {
        callback(cff::http::withRuntimeCorsHeaders(request, response));
    };
    const auto email = accountEmail(request);
    if (!email) {
        respond(errorResponse("Authentication is required.", "authentication_required", drogon::k401Unauthorized));
        return;
    }
    auto connection = connectDb();
    if (!connection) {
        respond(errorResponse("Lineup lock state is temporarily unavailable.", "lineup_lock_unavailable",
                              drogon::k503ServiceUnavailable, true));
        return;
    }
    if (!canAccessLeague(connection.get(), leagueId, *email)) {
        respond(errorResponse("Active league membership is required.", "league_membership_required",
                              drogon::k403Forbidden));
        return;
    }
    const auto context = requestWeek(request, connection.get(), leagueId);
    auto payload = lockState(connection.get(), leagueId, *email, context);
    if (!payload["players"].isArray()) {
        respond(errorResponse("Lineup lock state could not be loaded.", "lineup_lock_unavailable",
                              drogon::k503ServiceUnavailable, true));
        return;
    }
    respond(jsonResponse(payload));
}

drogon::HttpResponsePtr lineupGameLockAdvice(const drogon::HttpRequestPtr &request) {
    if (request->getMethod() != drogon::Post) return nullptr;
    const auto leagueId = pathLeagueId(request->getPath(), "/roster/transactions");
    if (leagueId.empty()) return nullptr;
    const auto body = request->getJsonObject();
    if (!body || !body->isObject() || lower(body->get("action", "").asString()) != "slot") return nullptr;

    const auto config = cff::config::loadRuntimeConfig();
    const auto respond = [&request](const drogon::HttpResponsePtr &response) {
        return cff::http::withRuntimeCorsHeaders(request, response);
    };
    const auto email = accountEmail(request);
    if (!email) {
        return respond(errorResponse("Authentication is required.", "authentication_required",
                                     drogon::k401Unauthorized));
    }
    auto connection = connectDb();
    if (!connection) {
        return respond(errorResponse("Lineup lock validation is temporarily unavailable.",
                                     "lineup_lock_unavailable", drogon::k503ServiceUnavailable, true));
    }
    if (!canAccessLeague(connection.get(), leagueId, *email)) {
        return respond(errorResponse("Active league membership is required.", "league_membership_required",
                                     drogon::k403Forbidden));
    }

    const auto context = requestWeek(request, connection.get(), leagueId);
    if (weekLocked(connection.get(), leagueId, *email, context)) {
        Json::Value details(Json::objectValue);
        details["season"] = context.season;
        details["week"] = context.week;
        return respond(errorResponse("This weekly lineup is locked.", "lineup_locked",
                                     drogon::k409Conflict, false, details));
    }

    const auto playerId = trim(body->get("playerId", "").asString());
    const auto locks = playerLocks(connection.get(), leagueId, *email, context);
    if (!locks.isArray()) {
        return respond(errorResponse("Lineup lock validation is temporarily unavailable.",
                                     "lineup_lock_unavailable", drogon::k503ServiceUnavailable, true));
    }
    for (const auto &lock : locks) {
        if (lock.get("playerId", "").asString() != playerId || !lock.get("locked", false).asBool()) continue;
        Json::Value details(Json::objectValue);
        details["playerId"] = playerId;
        details["gameId"] = lock.get("gameId", "");
        details["gameStartTime"] = lock.get("gameStartTime", "");
        details["season"] = context.season;
        details["week"] = context.week;
        return respond(errorResponse("This player's game has started, so the player is locked for the week.",
                                     "player_game_started", drogon::k409Conflict, false, details));
    }
    return nullptr;
}

struct LineupGameLockInstaller {
    LineupGameLockInstaller() {
        drogon::app().registerHandler(
            "/api/leagues/{1}/lineup-locks",
            [](const drogon::HttpRequestPtr &request,
               Callback &&callback,
               const std::string &leagueId) {
                handleLineupLocks(request, std::move(callback), leagueId);
            },
            {drogon::Get});
        drogon::app().registerSyncAdvice(lineupGameLockAdvice);
    }
};

#if defined(__GNUC__)
__attribute__((init_priority(102)))
#endif
LineupGameLockInstaller lineupGameLockInstaller;

} // namespace
