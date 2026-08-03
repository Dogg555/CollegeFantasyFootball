#include <drogon/drogon.h>
#include <json/json.h>
#include <postgresql/libpq-fe.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

struct PgConnDeleter {
    void operator()(PGconn *connection) const {
        if (connection) PQfinish(connection);
    }
};

struct PgResultDeleter {
    void operator()(PGresult *result) const {
        if (result) PQclear(result);
    }
};

using PgConnPtr = std::unique_ptr<PGconn, PgConnDeleter>;
using PgResultPtr = std::unique_ptr<PGresult, PgResultDeleter>;
using Callback = std::function<void(const drogon::HttpResponsePtr &)>;

PgConnPtr connectToDb() {
    const char *url = std::getenv("DB_URL");
    if (!url || !*url) return nullptr;
    auto connection = PgConnPtr{PQconnectdb(url)};
    if (PQstatus(connection.get()) != CONNECTION_OK) return nullptr;
    return connection;
}

PgResultPtr execParams(PGconn *connection,
                       const std::string &sql,
                       const std::vector<std::string> &params) {
    std::vector<const char *> values;
    values.reserve(params.size());
    for (const auto &param : params) values.push_back(param.c_str());
    return PgResultPtr{PQexecParams(connection,
                                    sql.c_str(),
                                    static_cast<int>(values.size()),
                                    nullptr,
                                    values.data(),
                                    nullptr,
                                    nullptr,
                                    0)};
}

bool tuplesOk(PGresult *result) {
    return result && PQresultStatus(result) == PGRES_TUPLES_OK;
}

bool commandOk(PGresult *result) {
    return result && PQresultStatus(result) == PGRES_COMMAND_OK;
}

std::string cell(PGresult *result, int row, int column) {
    if (!result || PQgetisnull(result, row, column)) return "";
    return PQgetvalue(result, row, column);
}

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

std::optional<std::string> bearerToken(const drogon::HttpRequestPtr &request) {
    const auto header = request->getHeader("authorization");
    constexpr const char *prefix = "Bearer ";
    if (header.rfind(prefix, 0) != 0 || header.size() <= 7) return std::nullopt;
    return header.substr(7);
}

std::optional<std::string> accountEmail(const drogon::HttpRequestPtr &request,
                                        PGconn *connection) {
    const auto token = bearerToken(request);
    if (!token) return std::nullopt;
    auto result = execParams(connection,
                             "SELECT email FROM auth_tokens "
                             "WHERE token = encode(digest($1, 'sha256'), 'hex') "
                             "AND expires_at > NOW() LIMIT 1",
                             {*token});
    if (!tuplesOk(result.get()) || PQntuples(result.get()) == 0) return std::nullopt;
    return lower(trim(cell(result.get(), 0, 0)));
}

Json::Value errorPayload(const std::string &message,
                         const std::string &code) {
    Json::Value payload;
    payload["error"] = message;
    payload["code"] = code;
    return payload;
}

void sendJson(Callback &callback,
              const Json::Value &payload,
              drogon::HttpStatusCode status = drogon::k200OK) {
    auto response = drogon::HttpResponse::newHttpJsonResponse(payload);
    response->setStatusCode(status);
    callback(response);
}

bool validTeamName(const std::string &teamName) {
    if (teamName.size() < 2 || teamName.size() > 40) return false;
    return std::none_of(teamName.begin(), teamName.end(), [](unsigned char ch) {
        return std::iscntrl(ch) != 0;
    });
}

void handleTeamName(const drogon::HttpRequestPtr &request,
                    Callback &&callback,
                    const std::string &leagueId) {
    auto connection = connectToDb();
    if (!connection) {
        sendJson(callback,
                 errorPayload("League database is unavailable", "DATABASE_UNAVAILABLE"),
                 drogon::k503ServiceUnavailable);
        return;
    }

    const auto email = accountEmail(request, connection.get());
    if (!email) {
        sendJson(callback,
                 errorPayload("Authentication required", "AUTH_REQUIRED"),
                 drogon::k401Unauthorized);
        return;
    }

    const auto body = request->getJsonObject();
    const auto teamName = body && body->isObject()
        ? trim((*body).get("teamName", "").asString())
        : "";
    if (!validTeamName(teamName)) {
        sendJson(callback,
                 errorPayload("Team names must be between 2 and 40 characters", "INVALID_TEAM_NAME"),
                 drogon::k400BadRequest);
        return;
    }

    auto membership = execParams(connection.get(),
        "SELECT role, status FROM league_members "
        "WHERE league_id = $1 AND email = $2 LIMIT 1",
        {leagueId, *email});
    if (!tuplesOk(membership.get()) || PQntuples(membership.get()) == 0) {
        sendJson(callback,
                 errorPayload("Join this league before choosing a team name", "MEMBERSHIP_REQUIRED"),
                 drogon::k404NotFound);
        return;
    }

    const auto role = cell(membership.get(), 0, 0);
    const auto status = cell(membership.get(), 0, 1);
    if (status == "removed") {
        sendJson(callback,
                 errorPayload("This membership has been removed", "MEMBERSHIP_REMOVED"),
                 drogon::k403Forbidden);
        return;
    }

    auto duplicate = execParams(connection.get(),
        "SELECT 1 FROM league_members "
        "WHERE league_id = $1 AND email <> $2 AND status <> 'removed' "
        "AND lower(btrim(team_name)) = lower(btrim($3)) LIMIT 1",
        {leagueId, *email, teamName});
    if (!tuplesOk(duplicate.get())) {
        sendJson(callback,
                 errorPayload("Team name availability could not be checked", "DATABASE_ERROR"),
                 drogon::k500InternalServerError);
        return;
    }
    if (PQntuples(duplicate.get()) > 0) {
        sendJson(callback,
                 errorPayload("That team name is already used in this league", "TEAM_NAME_TAKEN"),
                 drogon::k409Conflict);
        return;
    }

    auto updated = execParams(connection.get(),
        "UPDATE league_members SET team_name = $3, updated_at = NOW() "
        "WHERE league_id = $1 AND email = $2 AND status <> 'removed'",
        {leagueId, *email, teamName});
    if (!commandOk(updated.get()) || std::string{PQcmdTuples(updated.get())} == "0") {
        sendJson(callback,
                 errorPayload("Team name was not saved", "TEAM_NAME_NOT_SAVED"),
                 drogon::k409Conflict);
        return;
    }

    Json::Value payload;
    payload["leagueId"] = leagueId;
    payload["email"] = *email;
    payload["role"] = role;
    payload["status"] = status == "active" ? "Active"
                        : status == "pending" ? "Pending"
                        : "Invited";
    payload["teamName"] = teamName;
    sendJson(callback, payload);
}

struct TeamNameInstaller {
    TeamNameInstaller() {
        drogon::app().registerHandler(
            "/api/leagues/{1}/team-name",
            [](const drogon::HttpRequestPtr &request,
               Callback &&callback,
               const std::string &leagueId) {
                handleTeamName(request, std::move(callback), leagueId);
            },
            {drogon::Put});
    }
};

TeamNameInstaller teamNameInstaller;

}  // namespace
