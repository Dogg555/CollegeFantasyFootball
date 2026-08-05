#include <drogon/drogon.h>
#include <json/json.h>
#include <postgresql/libpq-fe.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
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

Json::Value parseJson(const std::string &raw, Json::Value fallback) {
    if (raw.empty()) return fallback;
    Json::CharReaderBuilder builder;
    std::string errors;
    std::istringstream stream(raw);
    Json::Value value;
    return Json::parseFromStream(builder, stream, &value, &errors) ? value : fallback;
}

std::string writeJson(const Json::Value &value) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, value);
}

bool draftDateAtTopOfHour(const std::string &value) {
    if (value.empty()) return true;
    const auto marker = value.find('T');
    return marker != std::string::npos
        && value.size() >= marker + 6
        && value.substr(marker + 3, 2) == "00";
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

std::string canonicalEmail(std::string value) {
    return lower(trim(std::move(value)));
}

std::string normalizeJoinCode(const std::string &value) {
    std::string code;
    for (unsigned char ch : value) {
        if (std::isalnum(ch)) code.push_back(static_cast<char>(std::toupper(ch)));
    }
    return code;
}

std::string displayJoinCode(const std::string &code) {
    if (code.size() == 8) return code.substr(0, 4) + "-" + code.substr(4);
    return code;
}

Json::Value errorPayload(const std::string &message,
                         const std::string &code = "REQUEST_FAILED") {
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
    return canonicalEmail(cell(result.get(), 0, 0));
}

bool isCommissioner(PGconn *connection,
                    const std::string &email,
                    const std::string &leagueId) {
    auto result = execParams(connection,
                             "SELECT 1 FROM leagues WHERE id = $2 AND account_email = $1 "
                             "UNION ALL "
                             "SELECT 1 FROM league_members WHERE league_id = $2 AND email = $1 "
                             "AND role = 'commissioner' AND status = 'active' LIMIT 1",
                             {email, leagueId});
    return tuplesOk(result.get()) && PQntuples(result.get()) > 0;
}

bool canAccessLeague(PGconn *connection,
                     const std::string &email,
                     const std::string &leagueId) {
    auto result = execParams(connection,
                             "SELECT 1 FROM leagues l WHERE l.id = $2 AND (l.account_email = $1 OR EXISTS ("
                             "SELECT 1 FROM league_members m WHERE m.league_id = l.id "
                             "AND m.email = $1 AND m.status = 'active')) LIMIT 1",
                             {email, leagueId});
    return tuplesOk(result.get()) && PQntuples(result.get()) > 0;
}

Json::Value membersForLeague(PGconn *connection, const std::string &leagueId) {
    auto result = execParams(connection,
                             "SELECT email, role, status, COALESCE(invited_by_email, ''), "
                             "COALESCE(team_name, '') FROM league_members "
                             "WHERE league_id = $1 AND status <> 'removed' ORDER BY role, created_at",
                             {leagueId});
    Json::Value members(Json::arrayValue);
    if (!tuplesOk(result.get())) return members;
    for (int row = 0; row < PQntuples(result.get()); ++row) {
        Json::Value member;
        member["email"] = cell(result.get(), row, 0);
        member["role"] = cell(result.get(), row, 1);
        const auto status = cell(result.get(), row, 2);
        member["status"] = status == "active" ? "Active"
                            : status == "pending" ? "Pending"
                            : status == "removed" ? "Removed"
                            : "Invited";
        member["invitedByEmail"] = cell(result.get(), row, 3);
        member["teamName"] = cell(result.get(), row, 4);
        members.append(member);
    }
    return members;
}

std::optional<Json::Value> leaguePayload(PGconn *connection, const std::string &leagueId) {
    auto result = execParams(connection,
                             "SELECT id, name, team_count, scoring, scoring_settings::text, draft_type, "
                             "COALESCE(to_char(draft_date AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI'), ''), "
                             "(draft_lobby_open OR (draft_date IS NOT NULL AND draft_date <= NOW() + INTERVAL '30 minutes')), "
                             "COALESCE(to_char(draft_lobby_started_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI'), ''), "
                             "roster_rules::text, waiver_rules::text, trade_rules::text, notes, "
                             "to_json(invited_emails)::text, UPPER(SUBSTRING(MD5(id), 1, 8)) "
                             "FROM leagues WHERE id = $1 LIMIT 1",
                             {leagueId});
    if (!tuplesOk(result.get()) || PQntuples(result.get()) == 0) return std::nullopt;

    Json::Value payload;
    payload["id"] = cell(result.get(), 0, 0);
    payload["name"] = cell(result.get(), 0, 1);
    payload["teams"] = std::stoi(cell(result.get(), 0, 2));
    payload["scoring"] = cell(result.get(), 0, 3);
    payload["scoringLabel"] = payload["scoring"].asString() == "half_ppr" ? "Half-PPR"
                               : payload["scoring"].asString() == "standard" ? "Standard"
                               : "PPR";
    payload["scoringSettings"] = parseJson(cell(result.get(), 0, 4), Json::Value{Json::objectValue});
    payload["draftType"] = cell(result.get(), 0, 5);
    payload["draftTypeLabel"] = payload["draftType"].asString() == "auction" ? "Auction" : "Snake";
    payload["draftDate"] = cell(result.get(), 0, 6);
    payload["draftLobbyOpen"] = cell(result.get(), 0, 7) == "t";
    payload["draftLobbyStartedAt"] = cell(result.get(), 0, 8);
    payload["rosterRules"] = parseJson(cell(result.get(), 0, 9), Json::Value{Json::objectValue});
    payload["waiverRules"] = parseJson(cell(result.get(), 0, 10), Json::Value{Json::objectValue});
    payload["tradeRules"] = parseJson(cell(result.get(), 0, 11), Json::Value{Json::objectValue});
    payload["notes"] = cell(result.get(), 0, 12);
    payload["invitedEmails"] = parseJson(cell(result.get(), 0, 13), Json::Value{Json::arrayValue});
    payload["joinCode"] = displayJoinCode(cell(result.get(), 0, 14));
    payload["members"] = membersForLeague(connection, leagueId);
    return payload;
}

bool validTeams(int teams) {
    return teams >= 4 && teams <= 16 && teams % 2 == 0;
}

bool validScoring(const std::string &scoring) {
    return scoring == "ppr" || scoring == "half_ppr" || scoring == "standard";
}

bool validDraftType(const std::string &draftType) {
    return draftType == "snake" || draftType == "auction";
}

void handleSettings(const drogon::HttpRequestPtr &request,
                    Callback &&callback,
                    const std::string &leagueId) {
    auto connection = connectToDb();
    if (!connection) {
        sendJson(callback, errorPayload("League database is unavailable", "DATABASE_UNAVAILABLE"), drogon::k503ServiceUnavailable);
        return;
    }
    const auto email = accountEmail(request, connection.get());
    if (!email) {
        sendJson(callback, errorPayload("Authentication required", "AUTH_REQUIRED"), drogon::k401Unauthorized);
        return;
    }
    if (!isCommissioner(connection.get(), *email, leagueId)) {
        sendJson(callback, errorPayload("Commissioner access required", "COMMISSIONER_REQUIRED"), drogon::k403Forbidden);
        return;
    }

    const auto body = request->getJsonObject();
    if (!body || !body->isObject()) {
        sendJson(callback, errorPayload("A complete settings payload is required", "INVALID_SETTINGS"), drogon::k400BadRequest);
        return;
    }

    const auto name = trim((*body).get("name", "").asString());
    const auto teams = (*body).get("teams", 0).asInt();
    const auto scoring = (*body).get("scoring", "").asString();
    const auto draftType = (*body).get("draftType", "snake").asString();
    const auto draftDate = (*body).get("draftDate", "").asString();
    const auto notes = (*body).get("notes", "").asString();
    const auto lobbyStartedAt = (*body).get("draftLobbyStartedAt", "").asString();
    const bool lobbyOpen = (*body).get("draftLobbyOpen", false).asBool();

    if (name.empty() || name.size() > 120 || !validTeams(teams) || !validScoring(scoring) || !validDraftType(draftType)
        || !(*body).isMember("scoringSettings") || !(*body)["scoringSettings"].isObject()
        || !(*body).isMember("rosterRules") || !(*body)["rosterRules"].isObject()
        || !(*body).isMember("waiverRules") || !(*body)["waiverRules"].isObject()
        || !(*body).isMember("tradeRules") || !(*body)["tradeRules"].isObject()) {
        sendJson(callback, errorPayload("One or more league settings are invalid", "INVALID_SETTINGS"), drogon::k400BadRequest);
        return;
    }
    if (!draftDateAtTopOfHour(draftDate)) {
        sendJson(callback, errorPayload("Draft time must be scheduled at the top of an hour", "DRAFT_TIME_NOT_TOP_OF_HOUR"), drogon::k400BadRequest);
        return;
    }

    Json::Value invites(Json::arrayValue);
    if ((*body).isMember("invitedEmails") && (*body)["invitedEmails"].isArray()) {
        std::vector<std::string> unique;
        for (const auto &entry : (*body)["invitedEmails"]) {
            if (!entry.isString()) continue;
            auto invited = canonicalEmail(entry.asString());
            if (invited.empty() || invited == *email || std::find(unique.begin(), unique.end(), invited) != unique.end()) continue;
            unique.push_back(invited);
            invites.append(invited);
        }
    }

    auto begin = PgResultPtr{PQexec(connection.get(), "BEGIN")};
    if (!commandOk(begin.get())) {
        sendJson(callback, errorPayload("Could not start settings update", "DATABASE_ERROR"), drogon::k500InternalServerError);
        return;
    }

    const std::string updateSql =
        "UPDATE leagues SET name = $3, team_count = $4::int, scoring = $5, "
        "scoring_settings = $6::jsonb, draft_type = $7, "
        "draft_date = NULLIF($8, '')::timestamptz, draft_lobby_open = $9::boolean, "
        "draft_lobby_started_at = NULLIF($10, '')::timestamptz, roster_rules = $11::jsonb, "
        "waiver_rules = $12::jsonb, trade_rules = $13::jsonb, notes = $14, "
        "invited_emails = COALESCE(ARRAY(SELECT DISTINCT lower(btrim(invite.value)) "
        "FROM jsonb_array_elements_text($15::jsonb) AS invite(value) WHERE btrim(invite.value) <> ''), ARRAY[]::text[]), "
        "updated_at = NOW() WHERE id = $2 AND (account_email = $1 OR EXISTS ("
        "SELECT 1 FROM league_members WHERE league_id = $2 AND email = $1 "
        "AND role = 'commissioner' AND status = 'active'))";

    auto update = execParams(connection.get(), updateSql, {
        *email,
        leagueId,
        name,
        std::to_string(teams),
        scoring,
        writeJson((*body)["scoringSettings"]),
        draftType,
        draftDate,
        lobbyOpen ? "true" : "false",
        lobbyStartedAt,
        writeJson((*body)["rosterRules"]),
        writeJson((*body)["waiverRules"]),
        writeJson((*body)["tradeRules"]),
        notes,
        writeJson(invites)
    });

    if (!commandOk(update.get()) || std::string{PQcmdTuples(update.get())} == "0") {
        PQexec(connection.get(), "ROLLBACK");
        sendJson(callback, errorPayload("League settings were not updated", "SETTINGS_NOT_PERSISTED"), drogon::k409Conflict);
        return;
    }

    auto upsertInvites = execParams(connection.get(),
        "INSERT INTO league_members (league_id, email, role, status, invited_by_email) "
        "SELECT $1, invited.email, 'member', 'invited', $2 "
        "FROM unnest((SELECT invited_emails FROM leagues WHERE id = $1)) AS invited(email) "
        "WHERE invited.email <> $2 "
        "ON CONFLICT (league_id, email) DO UPDATE SET "
        "status = CASE WHEN league_members.status IN ('active', 'pending') THEN league_members.status ELSE 'invited' END, "
        "invited_by_email = EXCLUDED.invited_by_email, updated_at = NOW()",
        {leagueId, *email});
    auto removeOldInvites = execParams(connection.get(),
        "UPDATE league_members SET status = 'removed', updated_at = NOW() "
        "WHERE league_id = $1 AND role <> 'commissioner' AND status = 'invited' "
        "AND NOT (email = ANY(COALESCE((SELECT invited_emails FROM leagues WHERE id = $1), ARRAY[]::text[])))",
        {leagueId});

    if (!commandOk(upsertInvites.get()) || !commandOk(removeOldInvites.get())) {
        PQexec(connection.get(), "ROLLBACK");
        sendJson(callback, errorPayload("League invitations were not synchronized", "SETTINGS_NOT_PERSISTED"), drogon::k409Conflict);
        return;
    }

    auto commit = PgResultPtr{PQexec(connection.get(), "COMMIT")};
    if (!commandOk(commit.get())) {
        sendJson(callback, errorPayload("League settings could not be committed", "DATABASE_ERROR"), drogon::k500InternalServerError);
        return;
    }

    const auto saved = leaguePayload(connection.get(), leagueId);
    if (!saved) {
        sendJson(callback, errorPayload("Saved settings could not be read back", "SETTINGS_NOT_VERIFIED"), drogon::k500InternalServerError);
        return;
    }
    sendJson(callback, *saved);
}

void handleJoinInfo(const drogon::HttpRequestPtr &request,
                    Callback &&callback,
                    const std::string &leagueId) {
    auto connection = connectToDb();
    if (!connection) {
        sendJson(callback, errorPayload("League database is unavailable", "DATABASE_UNAVAILABLE"), drogon::k503ServiceUnavailable);
        return;
    }
    const auto email = accountEmail(request, connection.get());
    if (!email) {
        sendJson(callback, errorPayload("Authentication required", "AUTH_REQUIRED"), drogon::k401Unauthorized);
        return;
    }
    if (!canAccessLeague(connection.get(), *email, leagueId)) {
        sendJson(callback, errorPayload("League not found", "LEAGUE_NOT_FOUND"), drogon::k404NotFound);
        return;
    }
    const auto league = leaguePayload(connection.get(), leagueId);
    if (!league) {
        sendJson(callback, errorPayload("League not found", "LEAGUE_NOT_FOUND"), drogon::k404NotFound);
        return;
    }
    Json::Value payload;
    payload["leagueId"] = leagueId;
    payload["leagueName"] = (*league)["name"];
    payload["joinCode"] = (*league)["joinCode"];
    payload["requiresApproval"] = true;
    sendJson(callback, payload);
}

void handleJoin(const drogon::HttpRequestPtr &request, Callback &&callback) {
    auto connection = connectToDb();
    if (!connection) {
        sendJson(callback, errorPayload("League database is unavailable", "DATABASE_UNAVAILABLE"), drogon::k503ServiceUnavailable);
        return;
    }
    const auto email = accountEmail(request, connection.get());
    if (!email) {
        sendJson(callback, errorPayload("Authentication required", "AUTH_REQUIRED"), drogon::k401Unauthorized);
        return;
    }
    const auto body = request->getJsonObject();
    const auto rawCode = body && body->isObject() ? trim((*body).get("code", "").asString()) : "";
    const auto code = normalizeJoinCode(rawCode);
    if (rawCode.empty()) {
        sendJson(callback, errorPayload("A league join code is required", "JOIN_CODE_REQUIRED"), drogon::k400BadRequest);
        return;
    }

    auto leagueResult = execParams(connection.get(),
        "SELECT id, name, team_count, account_email, UPPER(SUBSTRING(MD5(id), 1, 8)) "
        "FROM leagues WHERE id = $1 OR UPPER(SUBSTRING(MD5(id), 1, 8)) = $2 LIMIT 1",
        {rawCode, code});
    if (!tuplesOk(leagueResult.get()) || PQntuples(leagueResult.get()) == 0) {
        sendJson(callback, errorPayload("That join code does not match a league", "JOIN_CODE_INVALID"), drogon::k404NotFound);
        return;
    }

    const auto leagueId = cell(leagueResult.get(), 0, 0);
    const auto leagueName = cell(leagueResult.get(), 0, 1);
    const int teamCount = std::stoi(cell(leagueResult.get(), 0, 2));
    const auto commissionerEmail = canonicalEmail(cell(leagueResult.get(), 0, 3));
    const auto joinCode = displayJoinCode(cell(leagueResult.get(), 0, 4));

    auto member = execParams(connection.get(),
        "SELECT role, status FROM league_members WHERE league_id = $1 AND email = $2 LIMIT 1",
        {leagueId, *email});
    if (tuplesOk(member.get()) && PQntuples(member.get()) > 0) {
        const auto status = cell(member.get(), 0, 1);
        if (status == "active") {
            const auto league = leaguePayload(connection.get(), leagueId);
            if (league) sendJson(callback, *league);
            else sendJson(callback, errorPayload("League not found", "LEAGUE_NOT_FOUND"), drogon::k404NotFound);
            return;
        }
        if (status == "pending") {
            Json::Value payload;
            payload["joinStatus"] = "pending_approval";
            payload["leagueId"] = leagueId;
            payload["leagueName"] = leagueName;
            payload["joinCode"] = joinCode;
            payload["message"] = "Your join request is already waiting for commissioner approval.";
            sendJson(callback, payload, drogon::k202Accepted);
            return;
        }
        if (status == "removed") {
            sendJson(callback, errorPayload("The commissioner removed this account from the league", "MEMBERSHIP_REMOVED"), drogon::k403Forbidden);
            return;
        }
        if (status == "invited") {
            auto activate = execParams(connection.get(),
                "UPDATE league_members SET status = 'active', joined_at = COALESCE(joined_at, NOW()), updated_at = NOW() "
                "WHERE league_id = $1 AND email = $2",
                {leagueId, *email});
            if (!commandOk(activate.get())) {
                sendJson(callback, errorPayload("The invitation could not be accepted", "JOIN_FAILED"), drogon::k500InternalServerError);
                return;
            }
            const auto league = leaguePayload(connection.get(), leagueId);
            if (league) sendJson(callback, *league);
            else sendJson(callback, errorPayload("League not found", "LEAGUE_NOT_FOUND"), drogon::k404NotFound);
            return;
        }
    }

    auto activeCount = execParams(connection.get(),
        "SELECT COUNT(*) FROM league_members WHERE league_id = $1 AND status = 'active'",
        {leagueId});
    const int activeMembers = tuplesOk(activeCount.get()) && PQntuples(activeCount.get()) > 0
        ? std::stoi(cell(activeCount.get(), 0, 0)) : teamCount;
    if (activeMembers >= teamCount) {
        sendJson(callback, errorPayload("This league is full", "LEAGUE_FULL"), drogon::k409Conflict);
        return;
    }

    auto requestJoin = execParams(connection.get(),
        "INSERT INTO league_members (league_id, email, role, status, invited_by_email) "
        "VALUES ($1, $2, 'member', 'pending', $3) "
        "ON CONFLICT (league_id, email) DO UPDATE SET "
        "status = CASE WHEN league_members.status = 'removed' THEN league_members.status ELSE 'pending' END, "
        "updated_at = NOW()",
        {leagueId, *email, commissionerEmail});
    if (!commandOk(requestJoin.get())) {
        sendJson(callback, errorPayload("The join request could not be saved", "JOIN_FAILED"), drogon::k500InternalServerError);
        return;
    }

    Json::Value payload;
    payload["joinStatus"] = "pending_approval";
    payload["leagueId"] = leagueId;
    payload["leagueName"] = leagueName;
    payload["joinCode"] = joinCode;
    payload["message"] = "Join request submitted. The commissioner must approve access.";
    sendJson(callback, payload, drogon::k202Accepted);
}

void handlePlayerPool(const drogon::HttpRequestPtr &request,
                      Callback &&callback,
                      const std::string &leagueId) {
    auto connection = connectToDb();
    if (!connection) {
        sendJson(callback, errorPayload("Player database is unavailable", "DATABASE_UNAVAILABLE"), drogon::k503ServiceUnavailable);
        return;
    }
    const auto email = accountEmail(request, connection.get());
    if (!email) {
        sendJson(callback, errorPayload("Authentication required", "AUTH_REQUIRED"), drogon::k401Unauthorized);
        return;
    }
    if (!canAccessLeague(connection.get(), *email, leagueId)) {
        sendJson(callback, errorPayload("League not found", "LEAGUE_NOT_FOUND"), drogon::k404NotFound);
        return;
    }

    auto result = execParams(connection.get(),
        "SELECT p.id, COALESCE(p.full_name, ''), COALESCE(p.team, ''), "
        "COALESCE(p.position, ''), COALESCE(p.conference, ''), COALESCE(p.year, ''), "
        "COALESCE(p.season, 0) FROM players p "
        "WHERE p.active = TRUE AND UPPER(COALESCE(p.position, '')) IN ('QB', 'RB', 'WR', 'TE', 'K') "
        "AND NOT EXISTS (SELECT 1 FROM rosters r WHERE r.league_id = $1 AND r.player_id = p.id) "
        "ORDER BY p.season DESC NULLS LAST, "
        "CASE UPPER(COALESCE(p.position, '')) WHEN 'QB' THEN 1 WHEN 'RB' THEN 2 "
        "WHEN 'WR' THEN 3 WHEN 'TE' THEN 4 WHEN 'K' THEN 5 ELSE 6 END, p.full_name "
        "LIMIT 500",
        {leagueId});

    if (!tuplesOk(result.get())) {
        sendJson(callback, errorPayload("Player pool could not be loaded", "PLAYER_POOL_UNAVAILABLE"), drogon::k503ServiceUnavailable);
        return;
    }

    Json::Value players(Json::arrayValue);
    for (int row = 0; row < PQntuples(result.get()); ++row) {
        Json::Value player;
        player["id"] = cell(result.get(), row, 0);
        player["name"] = cell(result.get(), row, 1);
        player["team"] = cell(result.get(), row, 2);
        player["position"] = cell(result.get(), row, 3);
        player["conference"] = cell(result.get(), row, 4);
        player["class"] = cell(result.get(), row, 5);
        player["season"] = std::stoi(cell(result.get(), row, 6));
        player["projection"] = 10.0;
        player["rank"] = row + 1;
        player["availability"] = "Free Agent";
        players.append(player);
    }
    sendJson(callback, players);
}

struct LeagueBetaStabilityInstaller {
    LeagueBetaStabilityInstaller() {
        auto &app = drogon::app();
        app.registerHandler("/api/leagues/{1}/settings", handleSettings, {drogon::Put, drogon::Post});
        app.registerHandler("/api/leagues/{1}/join-info", handleJoinInfo, {drogon::Get});
        app.registerHandler("/api/leagues/join", handleJoin, {drogon::Post});
        app.registerHandler("/api/leagues/{1}/player-pool", handlePlayerPool, {drogon::Get});
    }
};

LeagueBetaStabilityInstaller leagueBetaStabilityInstaller;

}  // namespace
