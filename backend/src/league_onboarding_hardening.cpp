#include <drogon/drogon.h>
#include <json/json.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#ifdef CFF_HAS_POSTGRES
#include <postgresql/libpq-fe.h>
#endif

#include "app_config.h"
#include "http_security.h"
#include "league_models.h"

namespace {

constexpr std::size_t kMaxOperationKeyLength = 128;
constexpr std::size_t kMaxLeagueNameLength = 80;
constexpr std::size_t kMaxInviteEmailLength = 254;

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

bool validEmail(const std::string &value) {
    const auto email = canonicalEmail(value);
    const auto at = email.find('@');
    return !email.empty()
        && email.size() <= kMaxInviteEmailLength
        && at > 0
        && at + 1 < email.size()
        && email.find(' ', 0) == std::string::npos;
}

bool allowedTeamCount(int teams) {
    static const std::unordered_set<int> allowed{4, 6, 8, 10, 12, 14, 16};
    return allowed.find(teams) != allowed.end();
}

std::string jsonString(const Json::Value &value,
                       const std::string &key,
                       const std::string &fallback = "") {
    return value.isObject() && value.isMember(key) && value[key].isString()
        ? value[key].asString()
        : fallback;
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
                                     drogon::HttpStatusCode status) {
    auto response = drogon::HttpResponse::newHttpJsonResponse(payload);
    response->setStatusCode(status);
    return response;
}

drogon::HttpResponsePtr errorResponse(drogon::HttpStatusCode status,
                                      const std::string &message,
                                      const std::string &code,
                                      bool retryable = false) {
    return jsonResponse(errorPayload(message, code, retryable), status);
}

std::string operationKey(const drogon::HttpRequestPtr &request) {
    auto value = trim(request->getHeader("Idempotency-Key"));
    if (value.empty()) value = trim(request->getHeader("X-Request-ID"));
    if (value.size() > kMaxOperationKeyLength) value.resize(kMaxOperationKeyLength);
    return value;
}

std::optional<std::string> accountEmail(const drogon::HttpRequestPtr &request) {
    const auto config = cff::config::loadRuntimeConfig();
    auto email = cff::http::accountEmailForRequest(request, config.jwtSecret);
    if (!email) return std::nullopt;
    return canonicalEmail(*email);
}

std::vector<std::string> normalizedInvites(const Json::Value &body,
                                           const std::string &ownerEmail) {
    std::vector<std::string> invites;
    std::unordered_set<std::string> seen;
    if (!body.isObject() || !body.isMember("invitedEmails") || !body["invitedEmails"].isArray()) {
        return invites;
    }
    for (const auto &item : body["invitedEmails"]) {
        if (!item.isString()) continue;
        const auto email = canonicalEmail(item.asString());
        if (!validEmail(email) || email == ownerEmail || !seen.insert(email).second) continue;
        invites.push_back(email);
    }
    return invites;
}

std::string pathLeagueId(const std::string &path,
                         const std::string &suffix = "") {
    const std::string prefix = "/api/leagues/";
    if (path.rfind(prefix, 0) != 0) return "";
    const auto start = prefix.size();
    if (!suffix.empty()) {
        if (path.size() <= suffix.size() || path.substr(path.size() - suffix.size()) != suffix) return "";
        return path.substr(start, path.size() - start - suffix.size());
    }
    const auto slash = path.find('/', start);
    return path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
}

#ifdef CFF_HAS_POSTGRES

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

bool dbConfigured() {
    const auto *value = std::getenv("DB_URL");
    return value && *value;
}

PgConnection connectDb() {
    const auto *value = std::getenv("DB_URL");
    if (!value) return nullptr;
    PgConnection connection{PQconnectdb(value)};
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

bool commandOk(const PgResult &result) {
    return result && PQresultStatus(result.get()) == PGRES_COMMAND_OK;
}

std::string cell(PGresult *result, int row, int column) {
    return PQgetisnull(result, row, column) ? "" : PQgetvalue(result, row, column);
}

int cellInt(PGresult *result, int row, int column, int fallback = 0) {
    const auto value = cell(result, row, column);
    if (value.empty()) return fallback;
    try {
        return std::stoi(value);
    } catch (...) {
        return fallback;
    }
}

bool begin(PGconn *connection) {
    return commandOk(execute(connection, "BEGIN"));
}

bool commit(PGconn *connection) {
    return commandOk(execute(connection, "COMMIT"));
}

void rollback(PGconn *connection) {
    (void)execute(connection, "ROLLBACK");
}

bool lockKey(PGconn *connection, const std::string &key) {
    return tuplesOk(execute(connection,
                            "SELECT pg_advisory_xact_lock(hashtextextended($1, 0))",
                            {key}));
}

std::string statusForUi(const std::string &status) {
    if (status == "active") return "Active";
    if (status == "pending") return "Pending";
    if (status == "removed") return "Removed";
    return "Invited";
}

Json::Value membersForLeague(PGconn *connection, const std::string &leagueId) {
    auto result = execute(connection,
                          "SELECT email, role, status, COALESCE(invited_by_email, ''), "
                          "COALESCE(team_name, ''), "
                          "COALESCE(to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), '') "
                          "FROM league_members WHERE league_id = $1 AND status <> 'removed' "
                          "ORDER BY CASE role WHEN 'commissioner' THEN 0 ELSE 1 END, created_at, email",
                          {leagueId});
    Json::Value members(Json::arrayValue);
    if (!tuplesOk(result)) return members;
    for (int row = 0; row < PQntuples(result.get()); ++row) {
        Json::Value member(Json::objectValue);
        member["email"] = cell(result.get(), row, 0);
        member["role"] = cell(result.get(), row, 1);
        member["status"] = statusForUi(cell(result.get(), row, 2));
        member["invitedByEmail"] = cell(result.get(), row, 3);
        member["teamName"] = cell(result.get(), row, 4);
        member["createdAt"] = cell(result.get(), row, 5);
        members.append(member);
    }
    return members;
}

std::optional<Json::Value> leaguePayload(PGconn *connection,
                                         const std::string &leagueId) {
    auto result = execute(connection,
                          "SELECT id, name, team_count, scoring, scoring_settings::text, draft_type, "
                          "COALESCE(to_char(draft_date AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), ''), "
                          "draft_lobby_open, "
                          "COALESCE(to_char(draft_lobby_started_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), ''), "
                          "roster_rules::text, waiver_rules::text, trade_rules::text, notes, "
                          "to_json(invited_emails)::text "
                          "FROM leagues WHERE id = $1",
                          {leagueId});
    if (!tuplesOk(result) || PQntuples(result.get()) == 0) return std::nullopt;
    Json::Value source(Json::objectValue);
    source["name"] = cell(result.get(), 0, 1);
    source["teams"] = cellInt(result.get(), 0, 2, 10);
    source["scoring"] = cell(result.get(), 0, 3);
    source["scoringSettings"] = jsonFromString(cell(result.get(), 0, 4));
    source["draftType"] = cell(result.get(), 0, 5);
    source["draftDate"] = cell(result.get(), 0, 6);
    source["draftLobbyOpen"] = cell(result.get(), 0, 7) == "t";
    source["draftLobbyStartedAt"] = cell(result.get(), 0, 8);
    source["rosterRules"] = jsonFromString(cell(result.get(), 0, 9));
    source["waiverRules"] = jsonFromString(cell(result.get(), 0, 10));
    source["tradeRules"] = jsonFromString(cell(result.get(), 0, 11));
    source["notes"] = cell(result.get(), 0, 12);
    source["invitedEmails"] = jsonFromString(cell(result.get(), 0, 13), Json::Value{Json::arrayValue});
    auto league = cff::League::fromJson(source);
    league.id = cell(result.get(), 0, 0);
    auto payload = league.toJson();
    payload["members"] = membersForLeague(connection, league.id);
    return payload;
}

bool commissioner(PGconn *connection,
                  const std::string &leagueId,
                  const std::string &email) {
    auto result = execute(connection,
                          "SELECT 1 FROM leagues WHERE id = $1 AND account_email = $2 "
                          "UNION SELECT 1 FROM league_members WHERE league_id = $1 AND email = $2 "
                          "AND role = 'commissioner' AND status = 'active' LIMIT 1",
                          {leagueId, email});
    return tuplesOk(result) && PQntuples(result.get()) > 0;
}

std::optional<Json::Value> createLeague(const drogon::HttpRequestPtr &request,
                                        const std::string &email,
                                        drogon::HttpStatusCode &status) {
    const auto body = request->getJsonObject();
    if (!body || !body->isObject()) {
        status = drogon::k400BadRequest;
        return errorPayload("League settings are required.", "league_payload_required");
    }
    const int teams = body->get("teams", 10).asInt();
    if (!allowedTeamCount(teams)) {
        status = drogon::k400BadRequest;
        return errorPayload("League size must be 4, 6, 8, 10, 12, 14, or 16 teams.", "unsupported_team_count");
    }
    const auto name = trim(jsonString(*body, "name", "New League"));
    if (name.empty() || name.size() > kMaxLeagueNameLength) {
        status = drogon::k400BadRequest;
        return errorPayload("League name must contain 1 to 80 characters.", "invalid_league_name");
    }
    auto invites = normalizedInvites(*body, email);
    if (invites.size() > static_cast<std::size_t>(teams - 1)) {
        status = drogon::k409Conflict;
        return errorPayload("The invite list exceeds the available manager slots.", "league_invite_capacity");
    }

    auto connection = connectDb();
    if (!connection || !begin(connection.get()) || !lockKey(connection.get(), "create:" + email)) {
        status = drogon::k503ServiceUnavailable;
        return errorPayload("League creation storage is temporarily unavailable.", "league_storage_unavailable", true);
    }

    const auto key = operationKey(request);
    if (!key.empty()) {
        auto replay = execute(connection.get(),
                              "SELECT id FROM leagues WHERE account_email = $1 AND creation_key = $2 LIMIT 1",
                              {email, key});
        if (!tuplesOk(replay)) {
            rollback(connection.get());
            status = drogon::k503ServiceUnavailable;
            return errorPayload("League creation storage is temporarily unavailable.", "league_storage_unavailable", true);
        }
        if (PQntuples(replay.get()) > 0) {
            const auto id = cell(replay.get(), 0, 0);
            auto payload = leaguePayload(connection.get(), id);
            if (!payload || !commit(connection.get())) {
                rollback(connection.get());
                status = drogon::k503ServiceUnavailable;
                return errorPayload("League creation could not be confirmed.", "league_confirmation_failed", true);
            }
            (*payload)["idempotentReplay"] = true;
            (*payload)["operationKey"] = key;
            (*payload)["message"] = "League creation already completed.";
            status = drogon::k200OK;
            return payload;
        }
    }

    auto count = execute(connection.get(),
                         "SELECT COUNT(*) FROM leagues WHERE account_email = $1",
                         {email});
    if (!tuplesOk(count)) {
        rollback(connection.get());
        status = drogon::k503ServiceUnavailable;
        return errorPayload("League creation storage is temporarily unavailable.", "league_storage_unavailable", true);
    }
    if (cellInt(count.get(), 0, 0, 0) >= 3) {
        rollback(connection.get());
        status = drogon::k409Conflict;
        return errorPayload("Each account can have up to three leagues.", "league_limit_reached");
    }

    Json::Value normalized = *body;
    normalized["name"] = name;
    normalized["teams"] = teams;
    normalized["invitedEmails"] = Json::Value{Json::arrayValue};
    for (const auto &invite : invites) normalized["invitedEmails"].append(invite);
    auto league = cff::League::fromJson(normalized);
    const auto leagueJson = league.toJson();

    auto inserted = execute(connection.get(),
                            "INSERT INTO leagues "
                            "(id, account_email, creation_key, name, team_count, scoring, scoring_settings, draft_type, "
                            "draft_date, draft_lobby_open, draft_lobby_started_at, roster_rules, waiver_rules, "
                            "trade_rules, notes, invited_emails) "
                            "VALUES ($1, $2, NULLIF($3, ''), $4, $5::int, $6, $7::jsonb, $8, "
                            "NULLIF($9, '')::timestamptz, $10::boolean, NULLIF($11, '')::timestamptz, "
                            "$12::jsonb, $13::jsonb, $14::jsonb, $15, "
                            "COALESCE(ARRAY(SELECT jsonb_array_elements_text($16::jsonb)), '{}'))",
                            {league.id,
                             email,
                             key,
                             league.name,
                             std::to_string(league.teams.teamCount),
                             league.scoring.id,
                             jsonToString(league.scoring.toJson()["scoringSettings"]),
                             league.draft.type,
                             league.draftDate,
                             league.draftLobbyOpen ? "true" : "false",
                             league.draftLobbyStartedAt,
                             jsonToString(league.rosterRules.toJson()),
                             jsonToString(leagueJson["waiverRules"]),
                             jsonToString(leagueJson["tradeRules"]),
                             league.notes,
                             jsonToString(leagueJson["invitedEmails"])});
    if (!commandOk(inserted)) {
        rollback(connection.get());
        status = drogon::k409Conflict;
        return errorPayload("League creation conflicted with another request. Retry using the same operation.", "league_create_conflict", true);
    }

    auto owner = execute(connection.get(),
                         "INSERT INTO league_members "
                         "(league_id, email, role, status, invited_by_email, joined_at) "
                         "VALUES ($1, $2, 'commissioner', 'active', $2, NOW()) "
                         "ON CONFLICT (league_id, email) DO UPDATE SET role = 'commissioner', status = 'active', "
                         "joined_at = COALESCE(league_members.joined_at, NOW()), updated_at = NOW()",
                         {league.id, email});
    if (!commandOk(owner)) {
        rollback(connection.get());
        status = drogon::k503ServiceUnavailable;
        return errorPayload("League membership setup failed.", "league_membership_setup_failed", true);
    }
    for (const auto &invite : invites) {
        auto member = execute(connection.get(),
                              "INSERT INTO league_members "
                              "(league_id, email, role, status, invited_by_email) "
                              "VALUES ($1, $2, 'member', 'invited', $3) "
                              "ON CONFLICT (league_id, email) DO NOTHING",
                              {league.id, invite, email});
        if (!commandOk(member)) {
            rollback(connection.get());
            status = drogon::k503ServiceUnavailable;
            return errorPayload("League invitation setup failed.", "league_invitation_setup_failed", true);
        }
    }

    auto payload = leaguePayload(connection.get(), league.id);
    if (!payload || !commit(connection.get())) {
        rollback(connection.get());
        status = drogon::k503ServiceUnavailable;
        return errorPayload("League creation could not be confirmed.", "league_confirmation_failed", true);
    }
    (*payload)["idempotentReplay"] = false;
    (*payload)["operationKey"] = key;
    (*payload)["message"] = "League created.";
    status = drogon::k201Created;
    return payload;
}

std::optional<Json::Value> joinLeague(const drogon::HttpRequestPtr &request,
                                      const std::string &email,
                                      const std::string &leagueId,
                                      drogon::HttpStatusCode &status) {
    auto connection = connectDb();
    if (!connection || !begin(connection.get()) || !lockKey(connection.get(), "join:" + leagueId)) {
        status = drogon::k503ServiceUnavailable;
        return errorPayload("League joining is temporarily unavailable.", "league_storage_unavailable", true);
    }
    auto league = execute(connection.get(),
                          "SELECT team_count, account_email, $2 = ANY(invited_emails) "
                          "FROM leagues WHERE id = $1",
                          {leagueId, email});
    if (!tuplesOk(league) || PQntuples(league.get()) == 0) {
        rollback(connection.get());
        status = drogon::k404NotFound;
        return errorPayload("League not found.", "league_not_found");
    }
    const int teamCount = cellInt(league.get(), 0, 0, 10);
    const bool listedInvite = cell(league.get(), 0, 2) == "t";
    auto member = execute(connection.get(),
                          "SELECT status FROM league_members WHERE league_id = $1 AND email = $2",
                          {leagueId, email});
    if (!tuplesOk(member)) {
        rollback(connection.get());
        status = drogon::k503ServiceUnavailable;
        return errorPayload("League joining is temporarily unavailable.", "league_storage_unavailable", true);
    }
    const auto currentStatus = PQntuples(member.get()) > 0 ? cell(member.get(), 0, 0) : "";
    if (currentStatus == "active") {
        auto payload = leaguePayload(connection.get(), leagueId);
        if (!payload || !commit(connection.get())) {
            rollback(connection.get());
            status = drogon::k503ServiceUnavailable;
            return errorPayload("League membership could not be confirmed.", "league_confirmation_failed", true);
        }
        (*payload)["joinStatus"] = "active";
        (*payload)["idempotentReplay"] = true;
        status = drogon::k200OK;
        return payload;
    }
    if (currentStatus == "pending") {
        Json::Value pending(Json::objectValue);
        pending["id"] = leagueId;
        pending["joinStatus"] = "pending_approval";
        pending["idempotentReplay"] = true;
        pending["message"] = "Your join request is already waiting for commissioner approval.";
        if (!commit(connection.get())) {
            rollback(connection.get());
            status = drogon::k503ServiceUnavailable;
            return errorPayload("League membership could not be confirmed.", "league_confirmation_failed", true);
        }
        status = static_cast<drogon::HttpStatusCode>(202);
        return pending;
    }
    if (!(listedInvite || currentStatus == "invited")) {
        rollback(connection.get());
        status = drogon::k403Forbidden;
        return errorPayload("This invitation does not belong to the signed-in account.", "invite_not_found");
    }

    auto reserved = execute(connection.get(),
                            "SELECT COUNT(*) FROM league_members WHERE league_id = $1 "
                            "AND status IN ('active', 'pending')",
                            {leagueId});
    if (!tuplesOk(reserved)) {
        rollback(connection.get());
        status = drogon::k503ServiceUnavailable;
        return errorPayload("League joining is temporarily unavailable.", "league_storage_unavailable", true);
    }
    const int reservedCount = cellInt(reserved.get(), 0, 0, 0);
    if (reservedCount >= teamCount) {
        rollback(connection.get());
        status = drogon::k409Conflict;
        return errorPayload("This league has no remaining manager slots.", "league_full");
    }

    auto upsert = execute(connection.get(),
                          "INSERT INTO league_members (league_id, email, role, status, invited_by_email) "
                          "VALUES ($1, $2, 'member', 'pending', NULL) "
                          "ON CONFLICT (league_id, email) DO UPDATE SET status = 'pending', updated_at = NOW()",
                          {leagueId, email});
    if (!commandOk(upsert) || !commit(connection.get())) {
        rollback(connection.get());
        status = drogon::k503ServiceUnavailable;
        return errorPayload("Join request could not be stored.", "join_request_failed", true);
    }
    Json::Value pending(Json::objectValue);
    pending["id"] = leagueId;
    pending["joinStatus"] = "pending_approval";
    pending["idempotentReplay"] = false;
    pending["reservedManagers"] = reservedCount + 1;
    pending["teamCount"] = teamCount;
    pending["message"] = "Join request submitted. A commissioner must approve access.";
    status = static_cast<drogon::HttpStatusCode>(202);
    return pending;
}

std::optional<Json::Value> inviteMember(const drogon::HttpRequestPtr &request,
                                        const std::string &email,
                                        const std::string &leagueId,
                                        drogon::HttpStatusCode &status) {
    const auto body = request->getJsonObject();
    const auto invitedEmail = body ? canonicalEmail(jsonString(*body, "email")) : "";
    if (!validEmail(invitedEmail)) {
        status = drogon::k400BadRequest;
        return errorPayload("A valid manager email is required.", "invalid_invite_email");
    }
    if (invitedEmail == email) {
        status = drogon::k409Conflict;
        return errorPayload("The commissioner is already a league member.", "member_already_exists");
    }
    auto connection = connectDb();
    if (!connection || !begin(connection.get()) || !lockKey(connection.get(), "join:" + leagueId)) {
        status = drogon::k503ServiceUnavailable;
        return errorPayload("League invitations are temporarily unavailable.", "league_storage_unavailable", true);
    }
    auto league = execute(connection.get(),
                          "SELECT team_count, account_email FROM leagues WHERE id = $1",
                          {leagueId});
    if (!tuplesOk(league) || PQntuples(league.get()) == 0) {
        rollback(connection.get());
        status = drogon::k404NotFound;
        return errorPayload("League not found.", "league_not_found");
    }
    if (!commissioner(connection.get(), leagueId, email)) {
        rollback(connection.get());
        status = drogon::k403Forbidden;
        return errorPayload("Commissioner access required.", "commissioner_required");
    }
    auto existing = execute(connection.get(),
                            "SELECT status FROM league_members WHERE league_id = $1 AND email = $2",
                            {leagueId, invitedEmail});
    if (!tuplesOk(existing)) {
        rollback(connection.get());
        status = drogon::k503ServiceUnavailable;
        return errorPayload("League invitations are temporarily unavailable.", "league_storage_unavailable", true);
    }
    if (PQntuples(existing.get()) > 0 && cell(existing.get(), 0, 0) != "removed") {
        auto members = membersForLeague(connection.get(), leagueId);
        if (!commit(connection.get())) {
            rollback(connection.get());
            status = drogon::k503ServiceUnavailable;
            return errorPayload("League invitations could not be confirmed.", "league_confirmation_failed", true);
        }
        status = drogon::k200OK;
        return members;
    }
    const int teamCount = cellInt(league.get(), 0, 0, 10);
    auto used = execute(connection.get(),
                        "SELECT COUNT(*) FROM league_members WHERE league_id = $1 AND status <> 'removed'",
                        {leagueId});
    if (!tuplesOk(used)) {
        rollback(connection.get());
        status = drogon::k503ServiceUnavailable;
        return errorPayload("League invitations are temporarily unavailable.", "league_storage_unavailable", true);
    }
    if (cellInt(used.get(), 0, 0, 0) >= teamCount) {
        rollback(connection.get());
        status = drogon::k409Conflict;
        return errorPayload("All manager slots are already invited or reserved.", "league_invite_capacity");
    }
    const auto ownerEmail = canonicalEmail(cell(league.get(), 0, 1));
    const auto requestedRole = body ? lower(jsonString(*body, "role", "member")) : "member";
    const auto safeRole = requestedRole == "commissioner" && email == ownerEmail ? "commissioner" : "member";
    auto saved = execute(connection.get(),
                         "INSERT INTO league_members (league_id, email, role, status, invited_by_email) "
                         "VALUES ($1, $2, $3, 'invited', $4) "
                         "ON CONFLICT (league_id, email) DO UPDATE SET role = EXCLUDED.role, status = 'invited', "
                         "invited_by_email = EXCLUDED.invited_by_email, updated_at = NOW()",
                         {leagueId, invitedEmail, safeRole, email});
    auto list = execute(connection.get(),
                        "UPDATE leagues SET invited_emails = ARRAY(SELECT DISTINCT unnest(invited_emails || ARRAY[$2])), "
                        "updated_at = NOW() WHERE id = $1",
                        {leagueId, invitedEmail});
    if (!commandOk(saved) || !commandOk(list)) {
        rollback(connection.get());
        status = drogon::k503ServiceUnavailable;
        return errorPayload("League invitation could not be stored.", "league_invitation_failed", true);
    }
    auto members = membersForLeague(connection.get(), leagueId);
    if (!commit(connection.get())) {
        rollback(connection.get());
        status = drogon::k503ServiceUnavailable;
        return errorPayload("League invitation could not be confirmed.", "league_confirmation_failed", true);
    }
    status = drogon::k201Created;
    return members;
}

std::optional<Json::Value> approveMember(const drogon::HttpRequestPtr &request,
                                         const std::string &email,
                                         const std::string &leagueId,
                                         const std::string &memberEmail,
                                         drogon::HttpStatusCode &status) {
    const auto body = request->getJsonObject();
    const auto requestedStatus = body ? lower(jsonString(*body, "status", "")) : "";
    if (requestedStatus != "active") return std::nullopt;
    const auto targetEmail = canonicalEmail(memberEmail);
    auto connection = connectDb();
    if (!connection || !begin(connection.get()) || !lockKey(connection.get(), "join:" + leagueId)) {
        status = drogon::k503ServiceUnavailable;
        return errorPayload("League approval is temporarily unavailable.", "league_storage_unavailable", true);
    }
    auto league = execute(connection.get(),
                          "SELECT team_count, account_email FROM leagues WHERE id = $1",
                          {leagueId});
    if (!tuplesOk(league) || PQntuples(league.get()) == 0) {
        rollback(connection.get());
        status = drogon::k404NotFound;
        return errorPayload("League not found.", "league_not_found");
    }
    if (!commissioner(connection.get(), leagueId, email)) {
        rollback(connection.get());
        status = drogon::k403Forbidden;
        return errorPayload("Commissioner access required.", "commissioner_required");
    }
    auto member = execute(connection.get(),
                          "SELECT role, status FROM league_members WHERE league_id = $1 AND email = $2",
                          {leagueId, targetEmail});
    if (!tuplesOk(member) || PQntuples(member.get()) == 0 || cell(member.get(), 0, 1) == "removed") {
        rollback(connection.get());
        status = drogon::k404NotFound;
        return errorPayload("Join request not found.", "join_request_not_found");
    }
    if (cell(member.get(), 0, 1) == "active") {
        auto members = membersForLeague(connection.get(), leagueId);
        if (!commit(connection.get())) {
            rollback(connection.get());
            status = drogon::k503ServiceUnavailable;
            return errorPayload("League approval could not be confirmed.", "league_confirmation_failed", true);
        }
        status = drogon::k200OK;
        return members;
    }
    const int teamCount = cellInt(league.get(), 0, 0, 10);
    auto active = execute(connection.get(),
                          "SELECT COUNT(*) FROM league_members WHERE league_id = $1 AND status = 'active'",
                          {leagueId});
    if (!tuplesOk(active)) {
        rollback(connection.get());
        status = drogon::k503ServiceUnavailable;
        return errorPayload("League approval is temporarily unavailable.", "league_storage_unavailable", true);
    }
    if (cellInt(active.get(), 0, 0, 0) >= teamCount) {
        rollback(connection.get());
        status = drogon::k409Conflict;
        return errorPayload("This league is full. Remove a manager before approving another request.", "league_full");
    }
    const auto ownerEmail = canonicalEmail(cell(league.get(), 0, 1));
    const auto requestedRole = body ? lower(jsonString(*body, "role", cell(member.get(), 0, 0))) : cell(member.get(), 0, 0);
    const auto safeRole = requestedRole == "commissioner" && email == ownerEmail ? "commissioner" : "member";
    const auto teamName = body ? jsonString(*body, "teamName", "") : "";
    auto updated = execute(connection.get(),
                           "UPDATE league_members SET status = 'active', role = $3, "
                           "team_name = CASE WHEN $4 <> '' THEN $4 ELSE team_name END, "
                           "joined_at = COALESCE(joined_at, NOW()), updated_at = NOW() "
                           "WHERE league_id = $1 AND email = $2 AND status IN ('invited', 'pending')",
                           {leagueId, targetEmail, safeRole, teamName});
    if (!commandOk(updated) || std::string{PQcmdTuples(updated.get())} != "1") {
        rollback(connection.get());
        status = drogon::k409Conflict;
        return errorPayload("The join request changed before approval. Refresh and try again.", "join_request_conflict", true);
    }
    auto members = membersForLeague(connection.get(), leagueId);
    if (!commit(connection.get())) {
        rollback(connection.get());
        status = drogon::k503ServiceUnavailable;
        return errorPayload("League approval could not be confirmed.", "league_confirmation_failed", true);
    }
    status = drogon::k200OK;
    return members;
}

#endif

drogon::HttpResponsePtr onboardingAdvice(const drogon::HttpRequestPtr &request) {
    const auto &path = request->getPath();
    const auto method = request->getMethod();
    const auto respond = [&request](const drogon::HttpResponsePtr &response) {
        return cff::http::withRuntimeCorsHeaders(request, response);
    };

    if (method == drogon::Post && path == "/api/leagues") {
        const auto body = request->getJsonObject();
        const int teams = body && body->isObject() ? body->get("teams", 10).asInt() : 10;
        if (!allowedTeamCount(teams)) {
            return respond(errorResponse(drogon::k400BadRequest,
                                         "League size must be 4, 6, 8, 10, 12, 14, or 16 teams.",
                                         "unsupported_team_count"));
        }
#ifdef CFF_HAS_POSTGRES
        if (dbConfigured()) {
            const auto email = accountEmail(request);
            if (!email) return nullptr;
            drogon::HttpStatusCode status = drogon::k500InternalServerError;
            auto payload = createLeague(request, *email, status);
            return payload ? respond(jsonResponse(*payload, status)) : nullptr;
        }
#endif
        return nullptr;
    }

#ifdef CFF_HAS_POSTGRES
    if (!dbConfigured()) return nullptr;
    const auto email = accountEmail(request);
    if (!email) return nullptr;

    if (method == drogon::Post && path.size() > 5 && path.substr(path.size() - 5) == "/join") {
        const auto leagueId = pathLeagueId(path, "/join");
        if (leagueId.empty()) return nullptr;
        drogon::HttpStatusCode status = drogon::k500InternalServerError;
        auto payload = joinLeague(request, *email, leagueId, status);
        return payload ? respond(jsonResponse(*payload, status)) : nullptr;
    }

    if (method == drogon::Post && path.size() > 8 && path.substr(path.size() - 8) == "/members") {
        const auto leagueId = pathLeagueId(path, "/members");
        if (leagueId.empty()) return nullptr;
        drogon::HttpStatusCode status = drogon::k500InternalServerError;
        auto payload = inviteMember(request, *email, leagueId, status);
        return payload ? respond(jsonResponse(*payload, status)) : nullptr;
    }

    if ((method == drogon::Put || method == drogon::Post) && path.find("/members/") != std::string::npos) {
        const auto leagueId = pathLeagueId(path);
        const auto marker = path.find("/members/");
        if (leagueId.empty() || marker == std::string::npos) return nullptr;
        const auto memberEmail = path.substr(marker + std::string{"/members/"}.size());
        drogon::HttpStatusCode status = drogon::k500InternalServerError;
        auto payload = approveMember(request, *email, leagueId, memberEmail, status);
        return payload ? respond(jsonResponse(*payload, status)) : nullptr;
    }
#endif

    return nullptr;
}

struct LeagueOnboardingInstaller {
    LeagueOnboardingInstaller() {
        drogon::app().registerSyncAdvice(onboardingAdvice);
    }
};

LeagueOnboardingInstaller leagueOnboardingInstaller;

} // namespace
