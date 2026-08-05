#include "league_handler.h"

#ifdef DROGON_FOUND
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <drogon/HttpResponse.h>
#include <json/json.h>
#ifdef CFF_HAS_POSTGRES
#include <postgresql/libpq-fe.h>
#endif
#include "../json_utils.h"
#include "../league_models.h"
#include "../league_schedule.h"
#include "../league_roster.h"
#include "../league_waiver.h"
#include "../league_trade.h"

namespace cff::handlers {

namespace {
// Helpers are defined later in this translation unit, but several early
// validation functions use them. Keep declarations here so the full source
// compiles without moving or duplicating implementation code.
std::string jsonString(const Json::Value &body,
                       const std::string &key,
                       const std::string &fallback = "");
Json::Value &arrayForLeague(std::unordered_map<std::string, Json::Value> &store,
                            const std::string &leagueId);

constexpr std::size_t kMaxLeaguesPerAccount = 3;

struct LeagueRecord {
    cff::League league;
    std::string ownerEmail;
};

std::mutex storeMutex;
std::unordered_map<std::string, LeagueRecord> leaguesById;
std::unordered_map<std::string, std::vector<std::string>> leagueIdsByOwner;
std::unordered_map<std::string, Json::Value> rostersByLeague;
std::unordered_map<std::string, Json::Value> waiversByLeague;
std::unordered_map<std::string, Json::Value> tradesByLeague;
std::unordered_map<std::string, Json::Value> transactionsByLeague;
std::unordered_map<std::string, Json::Value> feedPostsByLeague;
std::unordered_map<std::string, Json::Value> matchupsByLeague;
std::unordered_map<std::string, Json::Value> membersByLeague;
std::unordered_map<std::string, Json::Value> draftPicksByLeague;
std::unordered_map<std::string, Json::Value> draftOrdersByLeague;
std::unordered_map<std::string, Json::Value> draftQueuesByLeagueManager;
std::unordered_map<std::string, Json::Value> draftStateByLeague;

std::string lowerString(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string canonicalEmail(std::string value) {
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), value.end());
    return lowerString(std::move(value));
}

double projectionForPlayer(const Json::Value &player) {
    if (player.isMember("projection") && player["projection"].isNumeric()) {
        return player["projection"].asDouble();
    }
    if (player.isMember("projectedPoints") && player["projectedPoints"].isNumeric()) {
        return player["projectedPoints"].asDouble();
    }
    return 0.0;
}

std::string draftIsoTimestamp(std::chrono::system_clock::time_point value) {
    const auto raw = std::chrono::system_clock::to_time_t(value);
    std::tm timeInfo{};
#ifdef _WIN32
    gmtime_s(&timeInfo, &raw);
#else
    gmtime_r(&raw, &timeInfo);
#endif
    std::ostringstream out;
    out << std::put_time(&timeInfo, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

std::string draftIsoNow() {
    return draftIsoTimestamp(std::chrono::system_clock::now());
}

std::string draftDeadlineFromNow(int seconds) {
    return draftIsoTimestamp(std::chrono::system_clock::now() + std::chrono::seconds(std::max(1, seconds)));
}

Json::Value waiverRulesForLeagueLocked(const std::string &leagueId) {
    const auto it = leaguesById.find(leagueId);
    if (it != leaguesById.end() && it->second.league.waiverRules.isObject()) {
        return it->second.league.waiverRules;
    }
    Json::Value rules(Json::objectValue);
    rules["mode"] = "free_agency";
    rules["claimDeadline"] = "";
    rules["freeAgencyLocked"] = false;
    return rules;
}

Json::Value tradeRulesForLeagueLocked(const std::string &leagueId) {
    const auto it = leaguesById.find(leagueId);
    if (it != leaguesById.end() && it->second.league.tradeRules.isObject()) {
        return it->second.league.tradeRules;
    }
    Json::Value rules(Json::objectValue);
    rules["commissionerApproval"] = false;
    rules["expirationHours"] = 48;
    return rules;
}

Json::Value activeDraftOrderLocked(const std::string &leagueId) {
    Json::Value order(Json::arrayValue);
    for (const auto &member : arrayForLeague(membersByLeague, leagueId)) {
        if (lowerString(jsonString(member, "status")) != "active") continue;
        const auto email = canonicalEmail(jsonString(member, "email"));
        if (!email.empty()) order.append(email);
    }
    return order;
}

bool localDraftOrderMatchesActiveMembers(const std::string &leagueId,
                                         const Json::Value &order) {
    const auto active = activeDraftOrderLocked(leagueId);
    if (!order.isArray() || order.empty() || order.size() != active.size()) return false;
    std::unordered_set<std::string> activeEmails;
    for (const auto &email : active) activeEmails.insert(canonicalEmail(email.asString()));
    std::unordered_set<std::string> seen;
    for (const auto &value : order) {
        if (!value.isString()) return false;
        const auto email = canonicalEmail(value.asString());
        if (email.empty() || activeEmails.find(email) == activeEmails.end() || !seen.insert(email).second) {
            return false;
        }
    }
    return true;
}

Json::Value &localDraftStateLocked(const std::string &leagueId) {
    auto &state = draftStateByLeague[leagueId];
    if (!state.isObject()) {
        state = Json::Value{Json::objectValue};
        state["status"] = "not_started";
        state["currentPick"] = 1;
        state["pickClockSeconds"] = 90;
        state["pickDeadline"] = "";
        state["startedAt"] = "";
        state["draftOrder"] = activeDraftOrderLocked(leagueId);
    }
    if (!state["draftOrder"].isArray() || state["draftOrder"].empty()) {
        state["draftOrder"] = activeDraftOrderLocked(leagueId);
    }
    const auto leagueIt = leaguesById.find(leagueId);
    state["lobbyOpen"] = leagueIt != leaguesById.end() && leagueIt->second.league.draftLobbyOpen;
    state["draftType"] = leagueIt != leaguesById.end()
        ? leagueIt->second.league.draft.type
        : "snake";
    return state;
}

Json::Value localDraftPayloadLocked(const std::string &accountEmail,
                                    const std::string &leagueId) {
    auto &state = localDraftStateLocked(leagueId);
    Json::Value payload = state;
    payload["queue"] = arrayForLeague(draftQueuesByLeagueManager, leagueId + ":" + accountEmail);
    payload["picks"] = arrayForLeague(draftPicksByLeague, leagueId);
    payload["currentManager"] = cff::league_schedule::currentDraftManager(
        payload["draftOrder"],
        payload.get("currentPick", 1).asInt(),
        payload.get("draftType", "snake").asString());
    return payload;
}

bool lineupLockedLocked(const std::string &leagueId) {
    for (const auto &matchup : arrayForLeague(matchupsByLeague, leagueId)) {
        if (lowerString(jsonString(matchup, "status", "scheduled")) == "final") {
            return true;
        }
    }
    return false;
}

#ifdef CFF_HAS_POSTGRES
struct PgConnDeleter {
    void operator()(PGconn *conn) const {
        if (conn) {
            PQfinish(conn);
        }
    }
};

struct PgResultDeleter {
    void operator()(PGresult *res) const {
        if (res) {
            PQclear(res);
        }
    }
};

using PgConnPtr = std::unique_ptr<PGconn, PgConnDeleter>;
using PgResultPtr = std::unique_ptr<PGresult, PgResultDeleter>;

bool draftOrderMatchesMembers(PGconn *conn,
const std::string &leagueId,
const Json::Value &draftOrder);

bool dbConfigured() {
    const auto *url = std::getenv("DB_URL");
    return url && std::string{url}.size() > 0;
}

PgConnPtr connectToDb() {
    const auto *url = std::getenv("DB_URL");
    if (!url) {
        return nullptr;
    }
    auto conn = PgConnPtr{PQconnectdb(url)};
    if (PQstatus(conn.get()) != CONNECTION_OK) {
        std::cerr << "[leagues] Failed to connect to Postgres: " << PQerrorMessage(conn.get()) << std::endl;
        return nullptr;
    }
    return conn;
}

PgResultPtr execParams(PGconn *conn,
                       const std::string &sql,
                       const std::vector<std::string> &params) {
    std::vector<const char *> values;
    values.reserve(params.size());
    for (const auto &param : params) {
        values.push_back(param.c_str());
    }
    return PgResultPtr{PQexecParams(conn,
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

std::string cell(PGresult *result, int row, int col) {
    if (PQgetisnull(result, row, col)) {
        return "";
    }
    return PQgetvalue(result, row, col);
}

int cellInt(PGresult *result, int row, int col, int fallback) {
    const auto value = cell(result, row, col);
    if (value.empty()) {
        return fallback;
    }
    return std::stoi(value);
}

bool cellBool(PGresult *result, int row, int col) {
    return cell(result, row, col) == "t";
}

std::string jsonToString(const Json::Value &json) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, json);
}

Json::Value jsonFromString(const std::string &raw, Json::Value fallback = Json::Value{Json::objectValue}) {
    if (raw.empty()) {
        return fallback;
    }
    Json::CharReaderBuilder builder;
    std::string errors;
    std::istringstream stream(raw);
    Json::Value parsed;
    if (!Json::parseFromStream(builder, stream, &parsed, &errors)) {
        return fallback;
    }
    return parsed;
}

std::string statusForDb(std::string status) {
    std::transform(status.begin(), status.end(), status.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return status;
}

std::string statusForUi(const std::string &status) {
    if (status == "processed") return "Processed";
    if (status == "accepted") return "Accepted";
    if (status == "approved") return "Approved";
    if (status == "declined") return "Declined";
    if (status == "vetoed") return "Vetoed";
    if (status == "expired") return "Expired";
    if (status == "cancelled") return "Cancelled";
    if (status == "active") return "Active";
    if (status == "removed") return "Removed";
    if (status == "invited") return "Invited";
    if (status == "pending") return "Pending";
    return "Pending";
}

Json::Value membersForLeague(PGconn *conn, const std::string &leagueId) {
    auto result = execParams(conn,
                             "SELECT email, role, status, COALESCE(invited_by_email, ''), COALESCE(team_name, ''), "
                             "COALESCE(to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), '') "
                             "FROM league_members WHERE league_id = $1 AND status <> 'removed' ORDER BY role, created_at",
                             {leagueId});
    Json::Value members(Json::arrayValue);
    if (!resultOk(result.get(), PGRES_TUPLES_OK)) {
        return members;
    }
    for (int row = 0; row < PQntuples(result.get()); ++row) {
        Json::Value member;
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

Json::Value leagueJsonFromRow(PGresult *result, int row) {
    Json::Value league;
    const auto id = cell(result, row, 0);
    league["id"] = id;
    league["name"] = cell(result, row, 1);
    league["teams"] = cellInt(result, row, 2, 10);
    league["scoring"] = cell(result, row, 3);
    league["scoringSettings"] = jsonFromString(cell(result, row, 4));
    league["draftType"] = cell(result, row, 5);
    league["draftDate"] = cell(result, row, 6);
    league["draftLobbyOpen"] = cellBool(result, row, 7);
    league["draftLobbyStartedAt"] = cell(result, row, 8);
    league["rosterRules"] = jsonFromString(cell(result, row, 9));
    league["waiverRules"] = jsonFromString(cell(result, row, 10));
    league["tradeRules"] = jsonFromString(cell(result, row, 11));
    league["notes"] = cell(result, row, 12);
    league["invitedEmails"] = jsonFromString(cell(result, row, 13), Json::Value{Json::arrayValue});
    auto model = cff::League::fromJson(league);
    model.id = id;
    auto json = model.toJson();
    json["waiverRules"] = league["waiverRules"];
    json["tradeRules"] = league["tradeRules"];
    return json;
}

std::string leagueSelectSql(const std::string &whereClause) {
    return "SELECT id, name, team_count, scoring, scoring_settings::text, draft_type, "
           "COALESCE(to_char(draft_date AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), ''), "
           "(draft_lobby_open OR (draft_date IS NOT NULL AND draft_date <= NOW() + INTERVAL '30 minutes')), "
           "COALESCE(to_char(draft_lobby_started_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), ''), "
           "roster_rules::text, waiver_rules::text, trade_rules::text, notes, to_json(invited_emails)::text "
           "FROM leagues " + whereClause;
}

std::optional<Json::Value> dbGetLeague(const std::string &accountEmail, const std::string &leagueId) {
    auto conn = connectToDb();
    if (!conn) return std::nullopt;
    auto result = execParams(conn.get(),
                             leagueSelectSql("WHERE id = $2 AND (account_email = $1 OR EXISTS (SELECT 1 FROM league_members WHERE league_id = leagues.id AND email = $1 AND status = 'active'))"),
                             {accountEmail, leagueId});
    if (!resultOk(result.get(), PGRES_TUPLES_OK) || PQntuples(result.get()) == 0) {
        return std::nullopt;
    }
    auto league = leagueJsonFromRow(result.get(), 0);
    league["members"] = membersForLeague(conn.get(), leagueId);
    return league;
}

bool dbCanAccessLeague(const std::string &accountEmail, const std::string &leagueId) {
    return dbGetLeague(accountEmail, leagueId).has_value();
}

bool dbIsCommissioner(const std::string &accountEmail, const std::string &leagueId) {
    auto conn = connectToDb();
    if (!conn) return false;
    auto result = execParams(conn.get(),
                             "SELECT 1 FROM leagues WHERE id = $2 AND account_email = $1 "
                             "UNION SELECT 1 FROM league_members WHERE league_id = $2 AND email = $1 AND role = 'commissioner' AND status = 'active' LIMIT 1",
                             {accountEmail, leagueId});
    return resultOk(result.get(), PGRES_TUPLES_OK) && PQntuples(result.get()) > 0;
}

bool dbIsActiveMember(PGconn *conn, const std::string &leagueId, const std::string &memberEmail) {
    auto result = execParams(conn,
                             "SELECT 1 FROM league_members WHERE league_id = $1 AND email = $2 AND status = 'active' LIMIT 1",
                             {leagueId, memberEmail});
    return resultOk(result.get(), PGRES_TUPLES_OK) && PQntuples(result.get()) > 0;
}

bool dbIsActiveOrPendingMember(PGconn *conn, const std::string &leagueId, const std::string &memberEmail) {
    auto result = execParams(conn,
                             "SELECT 1 FROM league_members WHERE league_id = $1 AND email = $2 AND status IN ('active', 'pending') LIMIT 1",
                             {leagueId, memberEmail});
    return resultOk(result.get(), PGRES_TUPLES_OK) && PQntuples(result.get()) > 0;
}

bool dbUpsertMember(PGconn *conn,
                    const std::string &leagueId,
                    const std::string &email,
                    const std::string &role,
                    const std::string &status,
                    const std::string &invitedByEmail,
                    const std::string &teamName = "") {
    auto result = execParams(conn,
                             "INSERT INTO league_members (league_id, email, role, status, invited_by_email, team_name, joined_at) "
                             "VALUES ($1, $2, $3, $4, NULLIF($5, ''), $6, CASE WHEN $4 = 'active' THEN NOW() ELSE NULL END) "
                             "ON CONFLICT (league_id, email) DO UPDATE SET "
                             "role = EXCLUDED.role, status = EXCLUDED.status, invited_by_email = EXCLUDED.invited_by_email, "
                             "team_name = CASE WHEN EXCLUDED.team_name <> '' THEN EXCLUDED.team_name ELSE league_members.team_name END, "
                             "joined_at = CASE WHEN EXCLUDED.status = 'active' AND league_members.joined_at IS NULL THEN NOW() ELSE league_members.joined_at END, "
                             "updated_at = NOW()",
                             {leagueId, email, role, status, invitedByEmail, teamName});
    return resultOk(result.get(), PGRES_COMMAND_OK);
}

void dbSyncInvitedMembers(PGconn *conn,
                          const std::string &leagueId,
                          const std::string &commissionerEmail,
                          const Json::Value &invitedEmails) {
    if (!invitedEmails.isArray()) return;
    const auto normalizedCommissionerEmail = canonicalEmail(commissionerEmail);
    for (const auto &email : invitedEmails) {
        if (!email.isString()) continue;
        const auto memberEmail = canonicalEmail(email.asString());
        if (memberEmail.empty() || memberEmail == normalizedCommissionerEmail) continue;
        // League settings retain approved members in invitedEmails. Never demote an
        // active membership while synchronizing that compatibility list.
        if (dbIsActiveOrPendingMember(conn, leagueId, memberEmail)) continue;
        dbUpsertMember(conn, leagueId, memberEmail, "member", "invited", normalizedCommissionerEmail);
    }
}
#endif

Json::Value samplePlayer(const std::string &id,
                         const std::string &name,
                         const std::string &team,
                         const std::string &position,
                         const std::string &conference,
                         double projection,
                         int rank) {
    Json::Value player;
    player["id"] = id;
    player["name"] = name;
    player["team"] = team;
    player["position"] = position;
    player["conference"] = conference;
    player["projection"] = projection;
    player["rank"] = rank;
    return player;
}

Json::Value sampleFreeAgentPool() {
    Json::Value players(Json::arrayValue);
    players.append(samplePlayer("p-001", "Garrett Nussmeier", "LSU", "QB", "SEC", 24.8, 1));
    players.append(samplePlayer("p-002", "Jeremiyah Love", "Notre Dame", "RB", "Independent", 21.9, 2));
    players.append(samplePlayer("p-003", "Ryan Williams", "Alabama", "WR", "SEC", 20.7, 3));
    players.append(samplePlayer("p-004", "Cade Klubnik", "Clemson", "QB", "ACC", 23.1, 4));
    players.append(samplePlayer("p-005", "Nicholas Singleton", "Penn State", "RB", "Big Ten", 19.6, 5));
    players.append(samplePlayer("p-006", "Carnell Tate", "Ohio State", "WR", "Big Ten", 18.8, 6));
    players.append(samplePlayer("p-010", "Eli Stowers", "Vanderbilt", "TE", "SEC", 13.7, 10));
    return players;
}

Json::Value normalizeLeaguePayload(const Json::Value &body) {
    Json::Value normalized = body.isObject() ? body : Json::Value{Json::objectValue};
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
    if (!normalized.isMember("waiverRules") || !normalized["waiverRules"].isObject()) {
        Json::Value rules(Json::objectValue);
        rules["mode"] = "free_agency";
        rules["claimDeadline"] = "";
        rules["freeAgencyLocked"] = false;
        normalized["waiverRules"] = rules;
    }
    if (!normalized.isMember("tradeRules") || !normalized["tradeRules"].isObject()) {
        Json::Value rules(Json::objectValue);
        rules["commissionerApproval"] = false;
        rules["expirationHours"] = 48;
        normalized["tradeRules"] = rules;
    }
    return normalized;
}

bool draftDateAtTopOfHour(const std::string &value) {
    if (value.empty()) return true;
    const auto marker = value.find('T');
    return marker != std::string::npos
        && value.size() >= marker + 6
        && value.substr(marker + 3, 2) == "00";
}

Json::Value errorPayload(const std::string &message) {
    Json::Value error;
    error["error"] = message;
    return error;
}

drogon::HttpResponsePtr jsonResponse(const Json::Value &payload, drogon::HttpStatusCode status) {
    auto resp = drogon::HttpResponse::newHttpJsonResponse(payload);
    resp->setStatusCode(status);
    return resp;
}

void sendError(std::function<void (const drogon::HttpResponsePtr &)> &callback,
               drogon::HttpStatusCode status,
               const std::string &message) {
    callback(jsonResponse(errorPayload(message), status));
}

bool ownsLeagueLocked(const std::string &accountEmail, const std::string &leagueId) {
    const auto it = leaguesById.find(leagueId);
    return it != leaguesById.end() && it->second.ownerEmail == accountEmail;
}

bool isCommissionerLocked(const std::string &accountEmail, const std::string &leagueId) {
    if (ownsLeagueLocked(accountEmail, leagueId)) {
        return true;
    }
    const auto membersIt = membersByLeague.find(leagueId);
    if (membersIt == membersByLeague.end()) {
        return false;
    }
    for (const auto &member : membersIt->second) {
        if (cff::getStringOrDefault(member, "email") == accountEmail
            && cff::getStringOrDefault(member, "role") == "commissioner"
            && lowerString(cff::getStringOrDefault(member, "status", "Active")) == "active") {
            return true;
        }
    }
    return false;
}

bool canAccessLeagueLocked(const std::string &accountEmail, const std::string &leagueId) {
    if (ownsLeagueLocked(accountEmail, leagueId)) {
        return true;
    }
    const auto membersIt = membersByLeague.find(leagueId);
    if (membersIt == membersByLeague.end()) {
        return false;
    }
    for (const auto &member : membersIt->second) {
        if (cff::getStringOrDefault(member, "email") == accountEmail
            && lowerString(cff::getStringOrDefault(member, "status", "Active")) == "active") {
            return true;
        }
    }
    return false;
}

bool isActiveMemberLocked(const std::string &accountEmail, const std::string &leagueId) {
    const auto membersIt = membersByLeague.find(leagueId);
    if (membersIt == membersByLeague.end()) {
        return false;
    }
    for (const auto &member : membersIt->second) {
        if (cff::getStringOrDefault(member, "email") == accountEmail
            && lowerString(cff::getStringOrDefault(member, "status", "Active")) == "active") {
            return true;
        }
    }
    return false;
}

std::string jsonString(const Json::Value &body, const std::string &key, const std::string &fallback) {
    return cff::getStringOrDefault(body, key, fallback);
}

std::string timestampId(const std::string &prefix) {
    const auto now = std::chrono::system_clock::now().time_since_epoch().count();
    return prefix + "-" + std::to_string(now);
}

int currentSeasonYear() {
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm timeInfo{};
#ifdef _WIN32
    localtime_s(&timeInfo, &now);
#else
    localtime_r(&now, &timeInfo);
#endif
    return timeInfo.tm_year + 1900;
}

Json::Value normalizePlayerJson(const Json::Value &body) {
    Json::Value player = body.isObject() ? body : Json::Value{Json::objectValue};
    if (!player.isMember("id")) {
        player["id"] = timestampId("player");
    }
    if (!player.isMember("name")) {
        player["name"] = "Unknown player";
    }
    if (!player.isMember("position")) {
        player["position"] = "FLEX";
    }
    if (!player.isMember("team")) {
        player["team"] = "Team TBD";
    }
    return player;
}

double jsonDouble(const Json::Value &body, const std::string &key, double fallback) {
    const auto &node = body[key];
    return node.isNumeric() ? node.asDouble() : fallback;
}

std::string normalizedStatName(const std::string &value) {
    std::string normalized;
    normalized.reserve(value.size());
    for (const auto ch : lowerString(value)) {
        if (std::isalnum(static_cast<unsigned char>(ch))) {
            normalized.push_back(ch);
        }
    }
    return normalized;
}

bool statNameMatches(const std::string &name, const std::vector<std::string> &needles) {
    const auto normalized = normalizedStatName(name);
    for (const auto &needle : needles) {
        if (normalized.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

double fantasyPointsForStat(const Json::Value &settings,
                            const std::string &category,
                            const std::string &statName,
                            double value) {
    const auto cat = lowerString(category);
    if (cat == "passing") {
        if (statNameMatches(statName, {"passyards", "passingyards", "yds"})) {
            return value / jsonDouble(settings, "passingYardsPerPoint", 25.0);
        }
        if (statNameMatches(statName, {"passtd", "passingtd", "passingtouchdown", "touchdowns"})) {
            return value * jsonDouble(settings, "passingTd", 4.0);
        }
        if (statNameMatches(statName, {"interception", "interceptions", "int"})) {
            return value * jsonDouble(settings, "interception", -2.0);
        }
    }
    if (cat == "rushing") {
        if (statNameMatches(statName, {"rushyards", "rushingyards", "yds"})) {
            return value / jsonDouble(settings, "rushingYardsPerPoint", 10.0);
        }
        if (statNameMatches(statName, {"rushtd", "rushingtd", "rushingtouchdown", "touchdowns"})) {
            return value * jsonDouble(settings, "rushingTd", 6.0);
        }
    }
    if (cat == "receiving") {
        if (statNameMatches(statName, {"recyards", "receivingyards", "yds"})) {
            return value / jsonDouble(settings, "receivingYardsPerPoint", 10.0);
        }
        if (statNameMatches(statName, {"rectd", "receivingtd", "receivingtouchdown", "touchdowns"})) {
            return value * jsonDouble(settings, "receivingTd", 6.0);
        }
        if (statNameMatches(statName, {"reception", "receptions", "rec", "catches"})) {
            return value * jsonDouble(settings, "reception", 1.0);
        }
    }
    if (statNameMatches(statName, {"fumblelost", "fumbleslost"})) {
        return value * jsonDouble(settings, "fumbleLost", -2.0);
    }
    if (statNameMatches(statName, {"twopoint", "twopointconversion", "twopt"})) {
        return value * jsonDouble(settings, "twoPointConversion", 2.0);
    }
    return 0.0;
}

Json::Value &arrayForLeague(std::unordered_map<std::string, Json::Value> &store, const std::string &leagueId) {
    auto &arr = store[leagueId];
    if (!arr.isArray()) {
        arr = Json::Value{Json::arrayValue};
    }
    return arr;
}

void addTransactionLocked(const std::string &leagueId,
                          const std::string &type,
                          const std::string &summary,
                          const std::string &managerEmail) {
    auto &transactions = arrayForLeague(transactionsByLeague, leagueId);
    Json::Value txn;
    txn["id"] = timestampId("txn");
    txn["type"] = type;
    txn["summary"] = summary;
    txn["managerEmail"] = managerEmail;
    txn["createdAt"] = timestampId("at");
    transactions.insert(0, txn);
}

Json::Value feedItem(const std::string &type,
                     const std::string &summary,
                     const std::string &createdAt,
                     const std::string &managerEmail,
                     const std::string &badge) {
    Json::Value item;
    item["type"] = type;
    item["summary"] = summary;
    item["createdAt"] = createdAt.empty() ? timestampId("at") : createdAt;
    item["managerEmail"] = managerEmail;
    item["badge"] = badge;
    return item;
}

void sortFeedItems(Json::Value &items) {
    std::vector<Json::Value> sorted;
    for (const auto &item : items) {
        sorted.push_back(item);
    }
    std::sort(sorted.begin(), sorted.end(), [](const Json::Value &a, const Json::Value &b) {
        return jsonString(a, "createdAt") > jsonString(b, "createdAt");
    });
    items = Json::Value{Json::arrayValue};
    for (std::size_t i = 0; i < sorted.size() && i < 100; ++i) {
        items.append(sorted[i]);
    }
}

Json::Value buildLocalLeagueFeed(const std::string &leagueId) {
    Json::Value items(Json::arrayValue);
    for (const auto &post : arrayForLeague(feedPostsByLeague, leagueId)) {
        items.append(feedItem(jsonString(post, "type", "Commissioner Post"),
                              jsonString(post, "summary"),
                              jsonString(post, "createdAt"),
                              jsonString(post, "managerEmail"),
                              "Post"));
    }
    for (const auto &txn : arrayForLeague(transactionsByLeague, leagueId)) {
        items.append(feedItem(jsonString(txn, "type", "Transaction"),
                              jsonString(txn, "summary"),
                              jsonString(txn, "createdAt"),
                              jsonString(txn, "managerEmail"),
                              "Transaction"));
    }
    for (const auto &claim : arrayForLeague(waiversByLeague, leagueId)) {
        const auto status = jsonString(claim, "status", "Claim");
        const auto playerName = jsonString(claim["addPlayer"], "name", "player");
        items.append(feedItem("Waiver " + status,
                              status + ": " + playerName + (jsonString(claim, "dropPlayerId").empty() ? "" : " with a drop"),
                              jsonString(claim, "createdAt"),
                              jsonString(claim, "managerEmail"),
                              "Waiver"));
    }
    for (const auto &trade : arrayForLeague(tradesByLeague, leagueId)) {
        const auto status = jsonString(trade, "status", "Offer");
        const auto offerName = jsonString(trade["offerPlayer"], "name", "player");
        const auto requestName = jsonString(trade["requestPlayer"], "name", jsonString(trade, "requestPlayerName", "return"));
        items.append(feedItem("Trade " + status,
                              status + ": " + offerName + " for " + requestName,
                              jsonString(trade, "createdAt"),
                              jsonString(trade, "offeredByEmail"),
                              "Trade"));
    }
    for (const auto &matchup : arrayForLeague(matchupsByLeague, leagueId)) {
        if (lowerString(jsonString(matchup, "status")) != "final") continue;
        const auto homeScore = matchup["homeScore"].asDouble();
        const auto awayScore = matchup["awayScore"].asDouble();
        const auto winner = homeScore >= awayScore ? jsonString(matchup, "homeManager") : jsonString(matchup, "awayManager");
        const auto loser = homeScore >= awayScore ? jsonString(matchup, "awayManager") : jsonString(matchup, "homeManager");
        std::ostringstream summary;
        summary << winner << " beat " << loser << " " << std::fixed << std::setprecision(1)
                << std::max(homeScore, awayScore) << "-" << std::min(homeScore, awayScore) << ".";
        items.append(feedItem("Final Score", summary.str(), jsonString(matchup, "finalizedAt"), winner, "Final"));
    }
    sortFeedItems(items);
    return items;
}

int indexOfPlayer(const Json::Value &arr, const std::string &playerId) {
    for (Json::ArrayIndex i = 0; i < arr.size(); ++i) {
        if (arr[i].isObject() && jsonString(arr[i], "id") == playerId) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

Json::Value removePlayer(Json::Value &arr, const std::string &playerId) {
    Json::Value removed;
    Json::Value next(Json::arrayValue);
    for (const auto &item : arr) {
        if (item.isObject() && jsonString(item, "id") == playerId) {
            removed = item;
        } else {
            next.append(item);
        }
    }
    arr = next;
    return removed;
}

bool ensureLeagueAccess(std::function<void (const drogon::HttpResponsePtr &)> &callback,
                        const std::string &accountEmail,
                        const std::string &leagueId) {
    if (!canAccessLeagueLocked(accountEmail, leagueId)) {
        sendError(callback, drogon::k404NotFound, "League not found");
        return false;
    }
    return true;
}

bool ensureCommissionerAccess(std::function<void (const drogon::HttpResponsePtr &)> &callback,
                              const std::string &accountEmail,
                              const std::string &leagueId,
                              const std::string &message = "Commissioner access required") {
    if (!isCommissionerLocked(accountEmail, leagueId)) {
        sendError(callback, drogon::k403Forbidden, message);
        return false;
    }
    return true;
}

#ifdef CFF_HAS_POSTGRES
std::optional<Json::Value> dbCreateLeague(const std::string &accountEmail, const cff::League &league) {
    auto conn = connectToDb();
    if (!conn) return std::nullopt;
    const auto sql =
        "INSERT INTO leagues "
        "(id, account_email, name, team_count, scoring, scoring_settings, draft_type, draft_date, "
        "draft_lobby_open, draft_lobby_started_at, roster_rules, waiver_rules, trade_rules, notes, invited_emails) "
        "VALUES ($1, $2, $3, $4::int, $5, $6::jsonb, $7, NULLIF($8, '')::timestamptz, "
        "$9::boolean, NULLIF($10, '')::timestamptz, $11::jsonb, $12::jsonb, $13::jsonb, $14, "
        "COALESCE(ARRAY(SELECT jsonb_array_elements_text($15::jsonb)), '{}'))";
    const auto leagueJson = league.toJson();
    const std::vector<std::string> params{
        league.id,
        accountEmail,
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
        jsonToString(leagueJson["invitedEmails"])
    };
    auto result = execParams(conn.get(), sql, params);
    if (!resultOk(result.get(), PGRES_COMMAND_OK)) {
        std::cerr << "[leagues] create failed: " << PQerrorMessage(conn.get()) << std::endl;
        return std::nullopt;
    }
    dbUpsertMember(conn.get(), league.id, accountEmail, "commissioner", "active", accountEmail);
    dbSyncInvitedMembers(conn.get(), league.id, accountEmail, league.toJson()["invitedEmails"]);
    return dbGetLeague(accountEmail, league.id);
}

std::optional<Json::Value> dbListLeagues(const std::string &accountEmail) {
    auto conn = connectToDb();
    if (!conn) return std::nullopt;
    auto result = execParams(conn.get(),
                             leagueSelectSql("WHERE account_email = $1 OR EXISTS (SELECT 1 FROM league_members WHERE league_id = leagues.id AND email = $1 AND status = 'active') ORDER BY created_at DESC"),
                             {accountEmail});
    if (!resultOk(result.get(), PGRES_TUPLES_OK)) {
        std::cerr << "[leagues] list failed: " << PQerrorMessage(conn.get()) << std::endl;
        return std::nullopt;
    }
    Json::Value payload(Json::arrayValue);
    for (int row = 0; row < PQntuples(result.get()); ++row) {
        auto league = leagueJsonFromRow(result.get(), row);
        league["members"] = membersForLeague(conn.get(), league["id"].asString());
        payload.append(league);
    }
    return payload;
}

std::optional<Json::Value> dbUpdateLeague(const std::string &accountEmail,
                                          const std::string &leagueId,
                                          const cff::League &league) {
    auto conn = connectToDb();
    if (!conn) return std::nullopt;
    if (!dbIsCommissioner(accountEmail, leagueId)) return std::nullopt;
    const auto sql =
        "UPDATE leagues SET "
        "name = $3, team_count = $4::int, scoring = $5, scoring_settings = $6::jsonb, "
        "draft_type = $7, draft_date = NULLIF($8, '')::timestamptz, "
        "draft_lobby_open = $9::boolean, draft_lobby_started_at = NULLIF($10, '')::timestamptz, "
        "roster_rules = $11::jsonb, waiver_rules = $12::jsonb, trade_rules = $13::jsonb, notes = $14, "
        "invited_emails = COALESCE(ARRAY(SELECT jsonb_array_elements_text($15::jsonb)), '{}'), "
        "updated_at = NOW() "
        "WHERE id = $2 AND (account_email = $1 OR EXISTS ("
        "SELECT 1 FROM league_members WHERE league_id = $2 AND email = $1 AND role = 'commissioner' AND status = 'active'"
        "))";
    const auto leagueJson = league.toJson();
    const std::vector<std::string> params{
        accountEmail,
        leagueId,
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
        jsonToString(leagueJson["invitedEmails"])
    };
    auto result = execParams(conn.get(), sql, params);
    if (!resultOk(result.get(), PGRES_COMMAND_OK)) {
        std::cerr << "[leagues] update failed: " << PQerrorMessage(conn.get()) << std::endl;
        return std::nullopt;
    }
    dbSyncInvitedMembers(conn.get(), leagueId, accountEmail, league.toJson()["invitedEmails"]);
    return dbGetLeague(accountEmail, leagueId);
}

std::optional<bool> dbDeleteLeague(const std::string &accountEmail, const std::string &leagueId) {
    auto conn = connectToDb();
    if (!conn) return std::nullopt;
    if (!dbIsCommissioner(accountEmail, leagueId)) return false;
    auto result = execParams(conn.get(),
                             "DELETE FROM leagues WHERE id = $2 AND (account_email = $1 OR EXISTS ("
                             "SELECT 1 FROM league_members WHERE league_id = $2 AND email = $1 AND role = 'commissioner' AND status = 'active'"
                             "))",
                             {accountEmail, leagueId});
    if (!resultOk(result.get(), PGRES_COMMAND_OK)) {
        std::cerr << "[leagues] delete failed: " << PQerrorMessage(conn.get()) << std::endl;
        return std::nullopt;
    }
    return std::string{PQcmdTuples(result.get())} != "0";
}

std::optional<Json::Value> dbListMembers(const std::string &accountEmail, const std::string &leagueId) {
    if (!dbCanAccessLeague(accountEmail, leagueId)) return std::nullopt;
    auto conn = connectToDb();
    if (!conn) return std::nullopt;
    return membersForLeague(conn.get(), leagueId);
}

std::optional<Json::Value> dbInviteMember(const std::string &accountEmail,
                                          const std::string &leagueId,
                                          const std::string &email,
                                          const std::string &role) {
    if (!dbIsCommissioner(accountEmail, leagueId)) return std::nullopt;
    auto conn = connectToDb();
    if (!conn) return std::nullopt;
    const auto safeRole = role == "commissioner" ? "commissioner" : "member";
    if (!dbUpsertMember(conn.get(), leagueId, email, safeRole, "invited", accountEmail)) return std::nullopt;
    auto invitedEmails = execParams(conn.get(),
                                    "UPDATE leagues SET invited_emails = ARRAY(SELECT DISTINCT unnest(invited_emails || ARRAY[$3])), updated_at = NOW() "
                                    "WHERE account_email = $1 AND id = $2",
                                    {accountEmail, leagueId, email});
    (void)invitedEmails;
    return membersForLeague(conn.get(), leagueId);
}

std::optional<Json::Value> dbUpdateMember(const std::string &accountEmail,
                                          const std::string &leagueId,
                                          const std::string &memberEmail,
                                          const std::string &role,
                                          const std::string &status,
                                          const std::string &teamName,
                                          bool updateTeamName) {
    if (!dbIsCommissioner(accountEmail, leagueId)) return std::nullopt;
    auto conn = connectToDb();
    if (!conn) return std::nullopt;
    auto owner = execParams(conn.get(), "SELECT account_email FROM leagues WHERE id = $1", {leagueId});
    if (!resultOk(owner.get(), PGRES_TUPLES_OK) || PQntuples(owner.get()) == 0) return std::nullopt;
    const auto ownerEmail = canonicalEmail(cell(owner.get(), 0, 0));
    const auto normalizedAccountEmail = canonicalEmail(accountEmail);
    const auto normalizedMemberEmail = canonicalEmail(memberEmail);
    const auto safeRole = role == "commissioner" ? "commissioner" : "member";
    auto safeStatus = statusForDb(status);
    if (!(safeStatus == "active" || safeStatus == "invited" || safeStatus == "pending" || safeStatus == "removed")) {
        safeStatus = "invited";
    }
    if (normalizedMemberEmail == ownerEmail) {
        if (!updateTeamName || safeRole != "commissioner" || safeStatus != "active") return std::nullopt;
        auto nameUpdate = execParams(conn.get(),
                                     "UPDATE league_members SET team_name = $3, updated_at = NOW() WHERE league_id = $1 AND email = $2",
                                     {leagueId, normalizedMemberEmail, teamName});
        if (!resultOk(nameUpdate.get(), PGRES_COMMAND_OK) || std::string{PQcmdTuples(nameUpdate.get())} != "1") {
            return std::nullopt;
        }
        return membersForLeague(conn.get(), leagueId);
    }
    if (safeRole == "commissioner" && normalizedAccountEmail != ownerEmail) return std::nullopt;
    if (!dbUpsertMember(conn.get(), leagueId, normalizedMemberEmail, safeRole, safeStatus, normalizedAccountEmail, teamName)) return std::nullopt;
    if (updateTeamName) {
        auto nameUpdate = execParams(conn.get(),
                                     "UPDATE league_members SET team_name = $3, updated_at = NOW() WHERE league_id = $1 AND email = $2",
                                     {leagueId, normalizedMemberEmail, teamName});
        if (!resultOk(nameUpdate.get(), PGRES_COMMAND_OK)) return std::nullopt;
    }
    if (safeStatus == "removed") {
        auto removedInvite = execParams(conn.get(),
                                        "UPDATE leagues SET invited_emails = array_remove(invited_emails, $3), updated_at = NOW() "
                                        "WHERE id = $2 AND EXISTS (SELECT 1 FROM league_members WHERE league_id = $2 AND email = $1 AND role = 'commissioner' AND status = 'active')",
                                        {normalizedAccountEmail, leagueId, normalizedMemberEmail});
        (void)removedInvite;
    }
    return membersForLeague(conn.get(), leagueId);
}

std::optional<Json::Value> dbJoinLeague(const std::string &accountEmail, const std::string &leagueId) {
    if (auto activeLeague = dbGetLeague(accountEmail, leagueId)) {
        (*activeLeague)["joinStatus"] = "active";
        return activeLeague;
    }
    auto conn = connectToDb();
    if (!conn) return std::nullopt;
    auto eligible = execParams(conn.get(),
                               "SELECT 1 FROM leagues WHERE id = $1 AND $2 = ANY(invited_emails) "
                               "UNION SELECT 1 FROM league_members WHERE league_id = $1 AND email = $2 AND status IN ('invited', 'pending') LIMIT 1",
                               {leagueId, accountEmail});
    if (!resultOk(eligible.get(), PGRES_TUPLES_OK) || PQntuples(eligible.get()) == 0) return std::nullopt;
    if (!dbUpsertMember(conn.get(), leagueId, accountEmail, "member", "pending", "")) return std::nullopt;
    Json::Value pending;
    pending["id"] = leagueId;
    pending["joinStatus"] = "pending_approval";
    pending["message"] = "Join request submitted. A commissioner must approve access.";
    return pending;
}

bool dbAddTransaction(PGconn *conn,
                      const std::string &leagueId,
                      const std::string &type,
                      const std::string &summary,
                      const std::string &managerEmail,
                      const Json::Value &metadata = Json::Value{Json::objectValue}) {
    auto result = execParams(conn,
                             "INSERT INTO transactions "
                             "(id, league_id, manager_email, transaction_type, summary, metadata) "
                             "VALUES ($1, $2, $3, $4, $5, $6::jsonb)",
                             {timestampId("txn"), leagueId, managerEmail, type, summary, jsonToString(metadata)});
    if (!resultOk(result.get(), PGRES_COMMAND_OK)) {
        std::cerr << "[leagues] transaction insert failed: " << PQerrorMessage(conn) << std::endl;
        return false;
    }
    return true;
}

Json::Value snapshotPlayer(const Json::Value &player, const std::string &playerId) {
    auto snapshot = normalizePlayerJson(player);
    snapshot["id"] = playerId.empty() ? jsonString(snapshot, "id") : playerId;
    return snapshot;
}

std::optional<int> dbRosterLimit(const std::string &leagueId) {
    auto conn = connectToDb();
    if (!conn) return std::nullopt;
    auto result = execParams(conn.get(),
                             "SELECT roster_rules::text FROM leagues WHERE id = $1",
                             {leagueId});
    if (!resultOk(result.get(), PGRES_TUPLES_OK) || PQntuples(result.get()) == 0) {
        return std::nullopt;
    }
    return cff::league_roster::rosterLimitFromRules(jsonFromString(cell(result.get(), 0, 0)));
}

bool dbRosterHasRoom(PGconn *conn, const std::string &leagueId, const std::string &managerEmail, int offset = 0) {
    auto limit = dbRosterLimit(leagueId).value_or(14);
    auto result = execParams(conn,
                             "SELECT COUNT(*) FROM rosters WHERE league_id = $1 AND manager_email = $2",
                             {leagueId, managerEmail});
    if (!resultOk(result.get(), PGRES_TUPLES_OK) || PQntuples(result.get()) == 0) {
        return false;
    }
    return cellInt(result.get(), 0, 0, 0) + offset < limit;
}

bool dbPlayerRosteredInLeague(PGconn *conn, const std::string &leagueId, const std::string &playerId) {
    auto result = execParams(conn,
                             "SELECT 1 FROM rosters WHERE league_id = $1 AND player_id = $2 LIMIT 1",
                             {leagueId, playerId});
    return resultOk(result.get(), PGRES_TUPLES_OK) && PQntuples(result.get()) > 0;
}

bool dbManagerHasPlayer(PGconn *conn,
                        const std::string &leagueId,
                        const std::string &managerEmail,
                        const std::string &playerId) {
    if (playerId.empty()) return false;
    auto result = execParams(conn,
                             "SELECT 1 FROM rosters WHERE league_id = $1 AND manager_email = $2 AND player_id = $3 LIMIT 1",
                             {leagueId, managerEmail, playerId});
    return resultOk(result.get(), PGRES_TUPLES_OK) && PQntuples(result.get()) > 0;
}

std::optional<Json::Value> dbRosterRules(const std::string &leagueId) {
    auto conn = connectToDb();
    if (!conn) return std::nullopt;
    auto result = execParams(conn.get(),
                             "SELECT roster_rules::text FROM leagues WHERE id = $1",
                             {leagueId});
    if (!resultOk(result.get(), PGRES_TUPLES_OK) || PQntuples(result.get()) == 0) {
        return std::nullopt;
    }
    return jsonFromString(cell(result.get(), 0, 0));
}

std::optional<Json::Value> dbWaiverRules(const std::string &leagueId) {
    auto conn = connectToDb();
    if (!conn) return std::nullopt;
    auto result = execParams(conn.get(),
                             "SELECT waiver_rules::text FROM leagues WHERE id = $1",
                             {leagueId});
    if (!resultOk(result.get(), PGRES_TUPLES_OK) || PQntuples(result.get()) == 0) {
        return std::nullopt;
    }
    return jsonFromString(cell(result.get(), 0, 0));
}

std::optional<Json::Value> dbTradeRules(const std::string &leagueId) {
    auto conn = connectToDb();
    if (!conn) return std::nullopt;
    auto result = execParams(conn.get(),
                             "SELECT trade_rules::text FROM leagues WHERE id = $1",
                             {leagueId});
    if (!resultOk(result.get(), PGRES_TUPLES_OK) || PQntuples(result.get()) == 0) {
        return std::nullopt;
    }
    return jsonFromString(cell(result.get(), 0, 0));
}

void dbExpireTrades(PGconn *conn, const std::string &leagueId) {
    auto result = execParams(conn,
                             "UPDATE trade_offers SET status = 'expired', resolved_at = NOW() "
                             "WHERE league_id = $1 AND status IN ('pending', 'accepted') AND expires_at IS NOT NULL AND expires_at < NOW()",
                             {leagueId});
    (void)result;
}

bool dbPlayerLockedInTrade(PGconn *conn,
                           const std::string &leagueId,
                           const std::string &managerEmail,
                           const std::string &playerId) {
    dbExpireTrades(conn, leagueId);
    auto result = execParams(conn,
                             "SELECT 1 FROM trade_offers "
                             "WHERE league_id = $1 AND status IN ('pending', 'accepted') "
                             "AND ((offered_by_email = $2 AND $3 = ANY(offered_player_ids)) "
                             "OR (offered_to_email = $2 AND $3 = ANY(requested_player_ids))) LIMIT 1",
                             {leagueId, managerEmail, playerId});
    return resultOk(result.get(), PGRES_TUPLES_OK) && PQntuples(result.get()) > 0;
}

std::optional<Json::Value> dbRosterPlayer(PGconn *conn,
                                          const std::string &leagueId,
                                          const std::string &managerEmail,
                                          const std::string &playerId) {
    if (playerId.empty()) return std::nullopt;
    auto result = execParams(conn,
                             "SELECT player_snapshot::text FROM rosters WHERE league_id = $1 AND manager_email = $2 AND player_id = $3",
                             {leagueId, managerEmail, playerId});
    if (!resultOk(result.get(), PGRES_TUPLES_OK) || PQntuples(result.get()) == 0) {
        return std::nullopt;
    }
    return normalizePlayerJson(jsonFromString(cell(result.get(), 0, 0)));
}

std::optional<std::string> dbAssignRosterSlot(PGconn *conn,
                                              const std::string &leagueId,
                                              const std::string &managerEmail,
                                              const Json::Value &player,
                                              int offset = 0) {
    const auto rules = dbRosterRules(leagueId).value_or(Json::Value{Json::objectValue});
    auto countsResult = execParams(conn,
                                   "SELECT roster_slot, COUNT(*) FROM rosters WHERE league_id = $1 AND manager_email = $2 GROUP BY roster_slot",
                                   {leagueId, managerEmail});
    std::unordered_map<std::string, int> counts;
    if (resultOk(countsResult.get(), PGRES_TUPLES_OK)) {
        for (int row = 0; row < PQntuples(countsResult.get()); ++row) {
            counts[lowerString(cell(countsResult.get(), row, 0))] = cellInt(countsResult.get(), row, 1, 0);
        }
    }
    return cff::league_roster::preferredRosterSlot(player, rules, counts, offset);
}

Json::Value draftPicksForLeague(PGconn *conn, const std::string &leagueId) {
    auto result = execParams(conn,
                             "SELECT id, manager_email, pick_number, player_id, player_snapshot::text, "
                             "COALESCE(to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), '') "
                             "FROM draft_picks WHERE league_id = $1 ORDER BY pick_number",
                             {leagueId});
    Json::Value picks(Json::arrayValue);
    if (!resultOk(result.get(), PGRES_TUPLES_OK)) {
        return picks;
    }
    for (int row = 0; row < PQntuples(result.get()); ++row) {
        Json::Value pick;
        pick["id"] = cell(result.get(), row, 0);
        pick["managerEmail"] = cell(result.get(), row, 1);
        pick["pickNumber"] = cellInt(result.get(), row, 2, row + 1);
        pick["player"] = snapshotPlayer(jsonFromString(cell(result.get(), row, 4)), cell(result.get(), row, 3));
        pick["createdAt"] = cell(result.get(), row, 5);
        picks.append(pick);
    }
    return picks;
}

Json::Value activeDraftOrderForLeague(PGconn *conn, const std::string &leagueId) {
    auto members = membersForLeague(conn, leagueId);
    Json::Value order(Json::arrayValue);
    for (const auto &member : members) {
        if (lowerString(jsonString(member, "status")) != "active") continue;
        const auto email = canonicalEmail(jsonString(member, "email"));
        if (!email.empty()) order.append(email);
    }
    return order;
}

Json::Value draftOrderForLeague(PGconn *conn, const std::string &leagueId) {
    auto result = execParams(conn,
                             "SELECT to_json(draft_order)::text FROM draft_states WHERE league_id = $1",
                             {leagueId});
    if (resultOk(result.get(), PGRES_TUPLES_OK) && PQntuples(result.get()) > 0) {
        const auto stored = jsonFromString(cell(result.get(), 0, 0), Json::Value{Json::arrayValue});
        if (stored.isArray() && !stored.empty()) return stored;
    }
    return activeDraftOrderForLeague(conn, leagueId);
}

bool draftOrderMatchesMembers(PGconn *conn, const std::string &leagueId, const Json::Value &draftOrder) {
    if (!draftOrder.isArray() || draftOrder.empty()) return false;
    std::unordered_set<std::string> memberEmails;
    for (const auto &member : membersForLeague(conn, leagueId)) {
        if (lowerString(jsonString(member, "status")) != "active") continue;
        memberEmails.insert(lowerString(jsonString(member, "email")));
    }
    if (memberEmails.empty() || draftOrder.size() != memberEmails.size()) return false;
    std::unordered_set<std::string> seen;
    for (const auto &emailValue : draftOrder) {
        if (!emailValue.isString()) return false;
        const auto email = lowerString(emailValue.asString());
        if (email.empty() || memberEmails.find(email) == memberEmails.end() || seen.find(email) != seen.end()) {
            return false;
        }
        seen.insert(email);
    }
    return true;
}

std::string draftTypeForLeague(PGconn *conn, const std::string &leagueId) {
    auto result = execParams(conn,
                             "SELECT draft_type FROM leagues WHERE id = $1",
                             {leagueId});
    if (resultOk(result.get(), PGRES_TUPLES_OK) && PQntuples(result.get()) > 0) {
        return lowerString(cell(result.get(), 0, 0));
    }
    return "snake";
}

bool dbDraftComplete(PGconn *conn, const std::string &leagueId) {
    const auto limit = dbRosterLimit(leagueId).value_or(14);
    auto members = membersForLeague(conn, leagueId);
    int activeMembers = 0;
    for (const auto &member : members) {
        if (lowerString(jsonString(member, "status")) != "active") continue;
        const auto email = jsonString(member, "email");
        if (email.empty()) continue;
        ++activeMembers;
        auto result = execParams(conn,
                                 "SELECT COUNT(*) FROM rosters WHERE league_id = $1 AND manager_email = $2",
                                 {leagueId, email});
        if (!resultOk(result.get(), PGRES_TUPLES_OK) || cellInt(result.get(), 0, 0, 0) < limit) {
            return false;
        }
    }
    return activeMembers > 0;
}

bool dbDraftLobbyOpen(PGconn *conn, const std::string &leagueId) {
    auto result = execParams(conn,
                             "SELECT draft_lobby_open OR (draft_date IS NOT NULL AND draft_date <= NOW() + INTERVAL '30 minutes') FROM leagues WHERE id = $1",
                             {leagueId});
    return resultOk(result.get(), PGRES_TUPLES_OK) && PQntuples(result.get()) > 0 && cellBool(result.get(), 0, 0);
}

bool dbLineupLocked(PGconn *conn, const std::string &leagueId) {
    auto result = execParams(conn,
                             "SELECT 1 FROM league_matchups WHERE league_id = $1 AND status = 'final' LIMIT 1",
                             {leagueId});
    return resultOk(result.get(), PGRES_TUPLES_OK) && PQntuples(result.get()) > 0;
}

std::optional<Json::Value> dbGetDraftState(const std::string &accountEmail, const std::string &leagueId) {
    if (!dbCanAccessLeague(accountEmail, leagueId)) return std::nullopt;
    auto conn = connectToDb();
    if (!conn) return std::nullopt;
    auto state = execParams(conn.get(),
                            "INSERT INTO draft_states (league_id, status, current_pick, draft_order, pick_deadline, started_at) "
                            "VALUES ($1, 'not_started', 1, ARRAY(SELECT email FROM league_members WHERE league_id = $1 AND status = 'active' ORDER BY role, created_at), NULL, NULL) "
                            "ON CONFLICT (league_id) DO NOTHING",
                            {leagueId});
    (void)state;
    auto queueResult = execParams(conn.get(),
                                  "SELECT queue::text FROM draft_queues WHERE league_id = $1 AND manager_email = $2",
                                  {leagueId, accountEmail});
    Json::Value payload;
    payload["queue"] = Json::Value{Json::arrayValue};
    if (resultOk(queueResult.get(), PGRES_TUPLES_OK) && PQntuples(queueResult.get()) > 0) {
        payload["queue"] = jsonFromString(cell(queueResult.get(), 0, 0), Json::Value{Json::arrayValue});
    }
    auto stateResult = execParams(conn.get(),
                                  "SELECT status, current_pick, pick_clock_seconds, "
                                  "COALESCE(to_char(pick_deadline AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), ''), "
                                  "COALESCE(to_char(started_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), '') "
                                  "FROM draft_states WHERE league_id = $1",
                                  {leagueId});
    payload["status"] = "not_started";
    payload["currentPick"] = 1;
    payload["pickClockSeconds"] = 90;
    payload["pickDeadline"] = "";
    payload["startedAt"] = "";
    if (resultOk(stateResult.get(), PGRES_TUPLES_OK) && PQntuples(stateResult.get()) > 0) {
        payload["status"] = cell(stateResult.get(), 0, 0);
        payload["currentPick"] = cellInt(stateResult.get(), 0, 1, 1);
        payload["pickClockSeconds"] = cellInt(stateResult.get(), 0, 2, 90);
        payload["pickDeadline"] = cell(stateResult.get(), 0, 3);
        payload["startedAt"] = cell(stateResult.get(), 0, 4);
    }
    payload["lobbyOpen"] = dbDraftLobbyOpen(conn.get(), leagueId);
    payload["draftType"] = draftTypeForLeague(conn.get(), leagueId);
    payload["draftOrder"] = draftOrderForLeague(conn.get(), leagueId);
    payload["currentManager"] = cff::league_schedule::currentDraftManager(
        payload["draftOrder"], payload["currentPick"].asInt(), payload["draftType"].asString());
    payload["picks"] = draftPicksForLeague(conn.get(), leagueId);
    if (payload["status"].asString() != "not_started" && dbDraftComplete(conn.get(), leagueId)) {
        payload["status"] = "complete";
    }
    return payload;
}

std::optional<Json::Value> dbStartDraft(const std::string &accountEmail,
                                          const std::string &leagueId) {
    if (!dbIsCommissioner(accountEmail, leagueId)) return std::nullopt;
    auto conn = connectToDb();
    if (!conn || !dbDraftLobbyOpen(conn.get(), leagueId)) return std::nullopt;
    auto insertState = execParams(conn.get(),
                                  "INSERT INTO draft_states (league_id, status, current_pick, draft_order, pick_deadline, started_at) "
                                  "VALUES ($1, 'not_started', 1, ARRAY(SELECT email FROM league_members WHERE league_id = $1 AND status = 'active' ORDER BY role, created_at), NULL, NULL) "
                                  "ON CONFLICT (league_id) DO NOTHING",
                                  {leagueId});
    (void)insertState;
    auto current = execParams(conn.get(),
                              "SELECT status FROM draft_states WHERE league_id = $1",
                              {leagueId});
    if (!resultOk(current.get(), PGRES_TUPLES_OK) || PQntuples(current.get()) == 0) return std::nullopt;
    const auto currentStatus = cell(current.get(), 0, 0);
    if (currentStatus == "open") return dbGetDraftState(accountEmail, leagueId);
    if (currentStatus != "not_started") return std::nullopt;
    auto order = draftOrderForLeague(conn.get(), leagueId);
    if (!draftOrderMatchesMembers(conn.get(), leagueId, order)) {
        order = activeDraftOrderForLeague(conn.get(), leagueId);
    }
    if (activeMemberCountForLeague(conn.get(), leagueId) < 2
    || order.size() < 2
    || !draftOrderMatchesMembers(conn.get(), leagueId, order)) return std::nullopt;
    auto picks = execParams(conn.get(),
                            "SELECT COUNT(*) FROM draft_picks WHERE league_id = $1",
                            {leagueId});
    if (!resultOk(picks.get(), PGRES_TUPLES_OK) || cellInt(picks.get(), 0, 0, 0) > 0) return std::nullopt;
    auto update = execParams(conn.get(),
                             "UPDATE draft_states SET status = 'open', current_pick = 1, "
                             "draft_order = ARRAY(SELECT jsonb_array_elements_text($2::jsonb)), "
                             "started_at = NOW(), pick_deadline = NOW() + (pick_clock_seconds * INTERVAL '1 second'), "
                             "updated_at = NOW() WHERE league_id = $1 AND status = 'not_started'",
                             {leagueId, jsonToString(order)});
    if (!resultOk(update.get(), PGRES_COMMAND_OK) || std::string{PQcmdTuples(update.get())} == "0") return std::nullopt;
    return dbGetDraftState(accountEmail, leagueId);
}

std::optional<Json::Value> dbSaveDraftQueue(const std::string &accountEmail,
                                            const std::string &leagueId,
                                            const Json::Value &queue) {
    if (!dbCanAccessLeague(accountEmail, leagueId)) return std::nullopt;
    auto conn = connectToDb();
    if (!conn) return std::nullopt;
    auto result = execParams(conn.get(),
                             "INSERT INTO draft_queues (league_id, manager_email, queue, updated_at) "
                             "VALUES ($1, $2, $3::jsonb, NOW()) "
                             "ON CONFLICT (league_id, manager_email) DO UPDATE SET queue = EXCLUDED.queue, updated_at = NOW()",
                             {leagueId, accountEmail, jsonToString(queue.isArray() ? queue : Json::Value{Json::arrayValue})});
    if (!resultOk(result.get(), PGRES_COMMAND_OK)) return std::nullopt;
    return dbGetDraftState(accountEmail, leagueId);
}

std::optional<Json::Value> dbSaveDraftOrder(const std::string &accountEmail,
                                            const std::string &leagueId,
                                            const Json::Value &draftOrder) {
    if (!dbIsCommissioner(accountEmail, leagueId)) return std::nullopt;
    auto conn = connectToDb();
    if (!conn) return std::nullopt;
    if (!draftOrderMatchesMembers(conn.get(), leagueId, draftOrder)) return std::nullopt;
    auto picks = execParams(conn.get(),
                            "SELECT COUNT(*) FROM draft_picks WHERE league_id = $1",
                            {leagueId});
    if (!resultOk(picks.get(), PGRES_TUPLES_OK) || cellInt(picks.get(), 0, 0, 0) > 0) return std::nullopt;
    auto insertState = execParams(conn.get(),
                                  "INSERT INTO draft_states (league_id, status, current_pick, draft_order, pick_deadline, started_at) "
                                  "VALUES ($1, 'not_started', 1, ARRAY(SELECT jsonb_array_elements_text($2::jsonb)), NULL, NULL) "
                                  "ON CONFLICT (league_id) DO NOTHING",
                                  {leagueId, jsonToString(draftOrder)});
    auto updateState = execParams(conn.get(),
                                  "UPDATE draft_states SET draft_order = ARRAY(SELECT jsonb_array_elements_text($2::jsonb)), "
                                  "current_pick = 1, pick_deadline = NULL, started_at = NULL, updated_at = NOW() "
                                  "WHERE league_id = $1 AND status = 'not_started'",
                                  {leagueId, jsonToString(draftOrder)});
    (void)insertState;
    if (!resultOk(updateState.get(), PGRES_COMMAND_OK) || std::string{PQcmdTuples(updateState.get())} == "0") return std::nullopt;
    return dbGetDraftState(accountEmail, leagueId);
}

std::optional<Json::Value> dbMakeDraftPick(const std::string &accountEmail,
                                           const std::string &leagueId,
                                           const Json::Value &player) {
    if (!dbCanAccessLeague(accountEmail, leagueId)) return std::nullopt;
    auto conn = connectToDb();
    if (!conn || !dbDraftLobbyOpen(conn.get(), leagueId)) return std::nullopt;
    auto state = execParams(conn.get(),
                            "SELECT status FROM draft_states WHERE league_id = $1",
                            {leagueId});
    if (!resultOk(state.get(), PGRES_TUPLES_OK) || PQntuples(state.get()) == 0) return std::nullopt;
    const auto currentStatus = cell(state.get(), 0, 0);
    if (currentStatus != "open") return std::nullopt;
    const auto normalized = normalizePlayerJson(player);
    if (jsonString(normalized, "id").empty()) return std::nullopt;
    auto order = draftOrderForLeague(conn.get(), leagueId);
    const auto draftType = draftTypeForLeague(conn.get(), leagueId);
    auto pickNumberResult = execParams(conn.get(),
                                       "SELECT COALESCE(MAX(pick_number), 0) + 1 FROM draft_picks WHERE league_id = $1",
                                       {leagueId});
    const auto pickNumber = resultOk(pickNumberResult.get(), PGRES_TUPLES_OK) && PQntuples(pickNumberResult.get()) > 0
                                ? cellInt(pickNumberResult.get(), 0, 0, 1)
                                : 1;
    const auto expectedManager = cff::league_schedule::currentDraftManager(order, pickNumber, draftType);
    if (!expectedManager.empty() && canonicalEmail(expectedManager) != canonicalEmail(accountEmail)) return std::nullopt;
    auto pickResult = execParams(conn.get(),
                                 "INSERT INTO draft_picks (id, league_id, manager_email, pick_number, player_id, player_snapshot) "
                                 "VALUES ($1, $2, $3, $4::int, $5, $6::jsonb)",
                                 {timestampId("pick"), leagueId, accountEmail, std::to_string(pickNumber), jsonString(normalized, "id"), jsonToString(normalized)});
    if (!resultOk(pickResult.get(), PGRES_COMMAND_OK)) return std::nullopt;
    const auto slot = dbAssignRosterSlot(conn.get(), leagueId, accountEmail, normalized);
    if (!slot) return std::nullopt;
    auto rosterResult = execParams(conn.get(),
                                   "INSERT INTO rosters (league_id, manager_email, player_id, player_snapshot, roster_slot, acquired_via) "
                                   "VALUES ($1, $2, $3, $4::jsonb, $5, 'draft') "
                                   "ON CONFLICT (league_id, manager_email, player_id) DO UPDATE SET player_snapshot = EXCLUDED.player_snapshot, roster_slot = EXCLUDED.roster_slot, acquired_via = 'draft'",
                                   {leagueId, accountEmail, jsonString(normalized, "id"), jsonToString(normalized), *slot});
    auto queueResult = execParams(conn.get(),
                                  "UPDATE draft_queues SET queue = COALESCE((SELECT jsonb_agg(item) FROM jsonb_array_elements(queue) item WHERE item->>'id' <> $3), '[]'::jsonb), updated_at = NOW() "
                                  "WHERE league_id = $1 AND manager_email = $2",
                                  {leagueId, accountEmail, jsonString(normalized, "id")});
    auto stateResult = execParams(conn.get(),
                                  "UPDATE draft_states SET current_pick = $2::int + 1, "
                                  "status = CASE WHEN $3 = 'true' THEN 'complete' ELSE status END, "
                                  "pick_deadline = CASE WHEN $3 = 'true' THEN NULL ELSE NOW() + (pick_clock_seconds * INTERVAL '1 second') END, "
                                  "updated_at = NOW() WHERE league_id = $1 AND status = 'open'",
                                  {leagueId, std::to_string(pickNumber), dbDraftComplete(conn.get(), leagueId) ? "true" : "false"});
    (void)queueResult;
    (void)stateResult;
    if (!resultOk(rosterResult.get(), PGRES_COMMAND_OK)) return std::nullopt;
    dbAddTransaction(conn.get(), leagueId, "Draft Pick", "Drafted " + jsonString(normalized, "name"), accountEmail, normalized);
    return dbGetDraftState(accountEmail, leagueId);
}

std::optional<Json::Value> dbResetDraft(const std::string &accountEmail, const std::string &leagueId) {
    if (!dbIsCommissioner(accountEmail, leagueId)) return std::nullopt;
    auto conn = connectToDb();
    if (!conn) return std::nullopt;
    auto picks = execParams(conn.get(), "DELETE FROM draft_picks WHERE league_id = $1", {leagueId});
    auto rosters = execParams(conn.get(), "DELETE FROM rosters WHERE league_id = $1 AND acquired_via = 'draft'", {leagueId});
    auto insertState = execParams(conn.get(),
                                  "INSERT INTO draft_states (league_id, status, current_pick, draft_order, pick_deadline, started_at) "
                                  "VALUES ($1, 'not_started', 1, ARRAY(SELECT email FROM league_members WHERE league_id = $1 AND status = 'active' ORDER BY role, created_at), NULL, NULL) "
                                  "ON CONFLICT (league_id) DO NOTHING",
                                  {leagueId});
    auto state = execParams(conn.get(),
                            "UPDATE draft_states SET current_pick = 1, status = 'not_started', "
                            "pick_deadline = NULL, started_at = NULL, updated_at = NOW() WHERE league_id = $1",
                            {leagueId});
    (void)picks;
    (void)rosters;
    (void)insertState;
    if (!resultOk(state.get(), PGRES_COMMAND_OK)) return std::nullopt;
    return dbGetDraftState(accountEmail, leagueId);
}

std::optional<Json::Value> dbUndoDraftPick(const std::string &accountEmail, const std::string &leagueId) {
    if (!dbIsCommissioner(accountEmail, leagueId)) return std::nullopt;
    auto conn = connectToDb();
    if (!conn) return std::nullopt;
    auto lastPick = execParams(conn.get(),
                               "SELECT manager_email, pick_number, player_id, player_snapshot::text "
                               "FROM draft_picks WHERE league_id = $1 ORDER BY pick_number DESC LIMIT 1",
                               {leagueId});
    if (!resultOk(lastPick.get(), PGRES_TUPLES_OK)) return std::nullopt;
    if (PQntuples(lastPick.get()) == 0) return dbGetDraftState(accountEmail, leagueId);

    const auto managerEmail = cell(lastPick.get(), 0, 0);
    const auto pickNumber = cellInt(lastPick.get(), 0, 1, 1);
    const auto playerId = cell(lastPick.get(), 0, 2);
    const auto playerSnapshot = cell(lastPick.get(), 0, 3);
    auto deletePick = execParams(conn.get(),
                                 "DELETE FROM draft_picks WHERE league_id = $1 AND pick_number = $2::int",
                                 {leagueId, std::to_string(pickNumber)});
    auto deleteRoster = execParams(conn.get(),
                                   "DELETE FROM rosters WHERE league_id = $1 AND manager_email = $2 AND player_id = $3 AND acquired_via = 'draft'",
                                   {leagueId, managerEmail, playerId});
    auto restoreQueue = execParams(conn.get(),
                                   "INSERT INTO draft_queues (league_id, manager_email, queue, updated_at) "
                                   "VALUES ($1, $2, jsonb_build_array($3::jsonb), NOW()) "
                                   "ON CONFLICT (league_id, manager_email) DO UPDATE SET "
                                   "queue = jsonb_build_array($3::jsonb) || "
                                   "COALESCE((SELECT jsonb_agg(item) FROM jsonb_array_elements(draft_queues.queue) item WHERE item->>'id' <> $4), '[]'::jsonb), "
                                   "updated_at = NOW()",
                                   {leagueId, managerEmail, playerSnapshot, playerId});
    auto state = execParams(conn.get(),
                            "UPDATE draft_states SET current_pick = $2::int, status = 'open', "
                            "pick_deadline = NOW() + (pick_clock_seconds * INTERVAL '1 second'), updated_at = NOW() "
                            "WHERE league_id = $1",
                            {leagueId, std::to_string(pickNumber)});
    (void)deleteRoster;
    (void)restoreQueue;
    (void)state;
    if (!resultOk(deletePick.get(), PGRES_COMMAND_OK)) return std::nullopt;
    dbAddTransaction(conn.get(), leagueId, "Draft Undo", "Undid pick " + std::to_string(pickNumber), accountEmail,
                     snapshotPlayer(jsonFromString(playerSnapshot), playerId));
    return dbGetDraftState(accountEmail, leagueId);
}

std::optional<Json::Value> dbGetRoster(const std::string &accountEmail, const std::string &leagueId) {
    if (!dbCanAccessLeague(accountEmail, leagueId)) return std::nullopt;
    auto conn = connectToDb();
    if (!conn) return std::nullopt;
    auto result = execParams(conn.get(),
                             "SELECT player_id, player_snapshot::text, roster_slot "
                             "FROM rosters WHERE league_id = $1 AND manager_email = $2 "
                             "ORDER BY acquired_at DESC",
                             {leagueId, accountEmail});
    if (!resultOk(result.get(), PGRES_TUPLES_OK)) return std::nullopt;
    Json::Value roster(Json::arrayValue);
    for (int row = 0; row < PQntuples(result.get()); ++row) {
        auto player = snapshotPlayer(jsonFromString(cell(result.get(), row, 1)), cell(result.get(), row, 0));
        player["rosterSlot"] = cell(result.get(), row, 2);
        roster.append(player);
    }
    return roster;
}

std::optional<Json::Value> dbGetManagerRoster(const std::string &accountEmail,
                                              const std::string &leagueId,
                                              const std::string &managerEmail) {
    if (!dbCanAccessLeague(accountEmail, leagueId)) return std::nullopt;
    auto conn = connectToDb();
    if (!conn) return std::nullopt;
    auto member = execParams(conn.get(),
                             "SELECT 1 FROM league_members WHERE league_id = $1 AND email = $2 AND status <> 'removed' "
                             "UNION SELECT 1 FROM leagues WHERE id = $1 AND account_email = $2 LIMIT 1",
                             {leagueId, managerEmail});
    if (!resultOk(member.get(), PGRES_TUPLES_OK) || PQntuples(member.get()) == 0) {
        return std::nullopt;
    }
    auto result = execParams(conn.get(),
                             "SELECT player_id, player_snapshot::text, roster_slot "
                             "FROM rosters WHERE league_id = $1 AND manager_email = $2 "
                             "ORDER BY acquired_at DESC",
                             {leagueId, managerEmail});
    if (!resultOk(result.get(), PGRES_TUPLES_OK)) return std::nullopt;
    Json::Value roster(Json::arrayValue);
    for (int row = 0; row < PQntuples(result.get()); ++row) {
        auto player = snapshotPlayer(jsonFromString(cell(result.get(), row, 1)), cell(result.get(), row, 0));
        player["rosterSlot"] = cell(result.get(), row, 2);
        roster.append(player);
    }
    return roster;
}

std::optional<Json::Value> dbAddRosterPlayer(const std::string &accountEmail,
                                             const std::string &leagueId,
                                             const Json::Value &player) {
    if (!dbCanAccessLeague(accountEmail, leagueId)) return std::nullopt;
    auto conn = connectToDb();
    if (!conn) return std::nullopt;
    if (dbLineupLocked(conn.get(), leagueId)) return std::nullopt;
    const auto normalized = normalizePlayerJson(player);
    const auto playerId = jsonString(normalized, "id");
    if (dbPlayerRosteredInLeague(conn.get(), leagueId, playerId)) return std::nullopt;
    const auto waiverRules = dbWaiverRules(leagueId).value_or(Json::Value{Json::objectValue});
    if (cff::league_waiver::modeActive(waiverRules)) return std::nullopt;
    if (!dbRosterHasRoom(conn.get(), leagueId, accountEmail)) return std::nullopt;
    const auto slot = dbAssignRosterSlot(conn.get(), leagueId, accountEmail, normalized);
    if (!slot) return std::nullopt;
    auto result = execParams(conn.get(),
                             "INSERT INTO rosters "
                             "(league_id, manager_email, player_id, player_snapshot, roster_slot, acquired_via) "
                             "VALUES ($1, $2, $3, $4::jsonb, $5, 'free_agency') "
                             "ON CONFLICT (league_id, manager_email, player_id) "
                             "DO UPDATE SET player_snapshot = EXCLUDED.player_snapshot, roster_slot = EXCLUDED.roster_slot",
                             {leagueId, accountEmail, playerId, jsonToString(normalized), *slot});
    if (!resultOk(result.get(), PGRES_COMMAND_OK)) return std::nullopt;
    dbAddTransaction(conn.get(), leagueId, "Free Agent", "Added " + jsonString(normalized, "name"), accountEmail, normalized);
    return dbGetRoster(accountEmail, leagueId);
}

std::optional<Json::Value> dbDropRosterPlayer(const std::string &accountEmail,
                                              const std::string &leagueId,
                                              const std::string &playerId) {
    if (!dbCanAccessLeague(accountEmail, leagueId)) return std::nullopt;
    auto conn = connectToDb();
    if (!conn) return std::nullopt;
    if (dbLineupLocked(conn.get(), leagueId)) return std::nullopt;
    if (dbPlayerLockedInTrade(conn.get(), leagueId, accountEmail, playerId)) return std::nullopt;
    auto result = execParams(conn.get(),
                             "DELETE FROM rosters "
                             "WHERE league_id = $1 AND manager_email = $2 AND player_id = $3 "
                             "RETURNING player_snapshot::text",
                             {leagueId, accountEmail, playerId});
    if (!resultOk(result.get(), PGRES_TUPLES_OK)) return std::nullopt;
    if (PQntuples(result.get()) > 0) {
        auto removed = snapshotPlayer(jsonFromString(cell(result.get(), 0, 0)), playerId);
        dbAddTransaction(conn.get(), leagueId, "Drop", "Dropped " + jsonString(removed, "name"), accountEmail, removed);
    }
    return dbGetRoster(accountEmail, leagueId);
}

std::optional<Json::Value> dbUpdateRosterSlot(const std::string &accountEmail,
                                              const std::string &leagueId,
                                              const std::string &playerId,
                                              const std::string &requestedSlot) {
    if (!dbCanAccessLeague(accountEmail, leagueId)) return std::nullopt;
    auto conn = connectToDb();
    if (!conn) return std::nullopt;
    if (dbLineupLocked(conn.get(), leagueId)) return std::nullopt;
    auto rosterResult = execParams(conn.get(),
                                   "SELECT player_id, player_snapshot::text, roster_slot "
                                   "FROM rosters WHERE league_id = $1 AND manager_email = $2 "
                                   "ORDER BY acquired_at DESC",
                                   {leagueId, accountEmail});
    if (!resultOk(rosterResult.get(), PGRES_TUPLES_OK)) return std::nullopt;
    Json::Value roster(Json::arrayValue);
    Json::Value target;
    for (int row = 0; row < PQntuples(rosterResult.get()); ++row) {
        auto player = snapshotPlayer(jsonFromString(cell(rosterResult.get(), row, 1)), cell(rosterResult.get(), row, 0));
        player["rosterSlot"] = cell(rosterResult.get(), row, 2);
        roster.append(player);
        if (jsonString(player, "id") == playerId) {
            target = player;
        }
    }
    if (!target.isObject()) return std::nullopt;
    const auto rules = dbRosterRules(leagueId).value_or(Json::Value{Json::objectValue});
    const auto slot = lowerString(requestedSlot);
    if (!cff::league_roster::validateRosterSlotMove(target, roster, rules, playerId, slot)) {
        Json::Value error;
        error["error"] = "Invalid roster slot";
        return error;
    }
    auto update = execParams(conn.get(),
                             "UPDATE rosters SET roster_slot = $4 WHERE league_id = $1 AND manager_email = $2 AND player_id = $3",
                             {leagueId, accountEmail, playerId, slot});
    if (!resultOk(update.get(), PGRES_COMMAND_OK)) return std::nullopt;
    return dbGetRoster(accountEmail, leagueId);
}

std::optional<Json::Value> dbFreeAgents(const std::string &accountEmail, const std::string &leagueId) {
    auto roster = dbGetRoster(accountEmail, leagueId);
    if (!roster) return std::nullopt;
    auto conn = connectToDb();
    if (!conn) return std::nullopt;
    Json::Value available(Json::arrayValue);
    for (const auto &player : sampleFreeAgentPool()) {
        if (dbPlayerRosteredInLeague(conn.get(), leagueId, jsonString(player, "id"))) {
            continue;
        }
        auto candidate = player;
        candidate["availability"] = "Free Agent";
        available.append(candidate);
    }
    return available;
}

std::optional<Json::Value> dbListWaivers(const std::string &accountEmail, const std::string &leagueId) {
    if (!dbCanAccessLeague(accountEmail, leagueId)) return std::nullopt;
    auto conn = connectToDb();
    if (!conn) return std::nullopt;
    const bool commissioner = dbIsCommissioner(accountEmail, leagueId);
    auto result = execParams(conn.get(),
                             "SELECT id, add_player_id, add_player_snapshot::text, COALESCE(drop_player_id, ''), "
                             "status, COALESCE(to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), ''), "
                             "manager_email, priority, claim_order "
                             "FROM waiver_claims WHERE league_id = $1 AND ($3 = 'true' OR manager_email = $2) "
                             "ORDER BY status = 'pending' DESC, priority ASC, claim_order ASC, created_at ASC",
                             {leagueId, accountEmail, commissioner ? "true" : "false"});
    if (!resultOk(result.get(), PGRES_TUPLES_OK)) return std::nullopt;
    Json::Value claims(Json::arrayValue);
    for (int row = 0; row < PQntuples(result.get()); ++row) {
        Json::Value claim;
        claim["id"] = cell(result.get(), row, 0);
        claim["addPlayer"] = snapshotPlayer(jsonFromString(cell(result.get(), row, 2)), cell(result.get(), row, 1));
        claim["dropPlayerId"] = cell(result.get(), row, 3);
        claim["status"] = statusForUi(cell(result.get(), row, 4));
        claim["createdAt"] = cell(result.get(), row, 5);
        claim["managerEmail"] = cell(result.get(), row, 6);
        claim["priority"] = cellInt(result.get(), row, 7, 1);
        claim["claimOrder"] = cellInt(result.get(), row, 8, row + 1);
        claims.append(claim);
    }
    return claims;
}

int dbPriorityForManager(PGconn *conn, const std::string &leagueId, const std::string &managerEmail) {
    auto seed = execParams(conn,
                           "INSERT INTO waiver_priorities (league_id, manager_email, priority) "
                           "VALUES ($1, $2, COALESCE((SELECT MAX(priority) + 1 FROM waiver_priorities WHERE league_id = $1), 1)) "
                           "ON CONFLICT (league_id, manager_email) DO NOTHING",
                           {leagueId, managerEmail});
    (void)seed;
    auto result = execParams(conn,
                             "SELECT priority FROM waiver_priorities WHERE league_id = $1 AND manager_email = $2",
                             {leagueId, managerEmail});
    if (!resultOk(result.get(), PGRES_TUPLES_OK) || PQntuples(result.get()) == 0) {
        return 1;
    }
    return cellInt(result.get(), 0, 0, 1);
}

void dbMoveManagerToBackOfWaivers(PGconn *conn, const std::string &leagueId, const std::string &managerEmail) {
    auto result = execParams(conn,
                             "UPDATE waiver_priorities SET priority = COALESCE((SELECT MAX(priority) + 1 FROM waiver_priorities WHERE league_id = $1), 1), updated_at = NOW() "
                             "WHERE league_id = $1 AND manager_email = $2",
                             {leagueId, managerEmail});
    (void)result;
}

Json::Value dbListWaiverPriority(PGconn *conn, const std::string &leagueId) {
    auto seed = execParams(conn,
                           "INSERT INTO waiver_priorities (league_id, manager_email, priority) "
                           "SELECT league_id, email, (ROW_NUMBER() OVER (ORDER BY role, created_at))::int "
                           "FROM league_members WHERE league_id = $1 AND status <> 'removed' "
                           "ON CONFLICT (league_id, manager_email) DO NOTHING",
                           {leagueId});
    (void)seed;
    auto result = execParams(conn,
                             "SELECT m.email, m.role, m.status, COALESCE(w.priority, 9999) "
                             "FROM league_members m "
                             "LEFT JOIN waiver_priorities w ON w.league_id = m.league_id AND w.manager_email = m.email "
                             "WHERE m.league_id = $1 AND m.status <> 'removed' "
                             "ORDER BY COALESCE(w.priority, 9999), m.created_at",
                             {leagueId});
    Json::Value priorities(Json::arrayValue);
    if (!resultOk(result.get(), PGRES_TUPLES_OK)) return priorities;
    for (int row = 0; row < PQntuples(result.get()); ++row) {
        Json::Value item;
        item["managerEmail"] = cell(result.get(), row, 0);
        item["role"] = cell(result.get(), row, 1);
        item["status"] = statusForUi(cell(result.get(), row, 2));
        item["priority"] = cellInt(result.get(), row, 3, row + 1);
        priorities.append(item);
    }
    return priorities;
}

std::optional<Json::Value> dbWaiverPriorityBoard(const std::string &accountEmail, const std::string &leagueId) {
    if (!dbCanAccessLeague(accountEmail, leagueId)) return std::nullopt;
    auto conn = connectToDb();
    if (!conn) return std::nullopt;
    return dbListWaiverPriority(conn.get(), leagueId);
}

std::optional<Json::Value> dbResetWaiverPriority(const std::string &accountEmail, const std::string &leagueId) {
    if (!dbIsCommissioner(accountEmail, leagueId)) return std::nullopt;
    auto conn = connectToDb();
    if (!conn) return std::nullopt;
    auto clear = execParams(conn.get(), "DELETE FROM waiver_priorities WHERE league_id = $1", {leagueId});
    (void)clear;
    auto seed = execParams(conn.get(),
                           "INSERT INTO waiver_priorities (league_id, manager_email, priority) "
                           "SELECT league_id, email, (ROW_NUMBER() OVER (ORDER BY role, created_at))::int "
                           "FROM league_members WHERE league_id = $1 AND status <> 'removed'",
                           {leagueId});
    if (!resultOk(seed.get(), PGRES_COMMAND_OK)) return std::nullopt;
    dbAddTransaction(conn.get(), leagueId, "Waiver Priority", cff::league_waiver::resetPriorityTransactionSummary(), accountEmail, Json::Value{Json::objectValue});
    return dbListWaiverPriority(conn.get(), leagueId);
}

std::optional<Json::Value> dbCreateWaiver(const std::string &accountEmail,
                                          const std::string &leagueId,
                                          const Json::Value &body) {
    if (!dbCanAccessLeague(accountEmail, leagueId)) return std::nullopt;
    auto conn = connectToDb();
    if (!conn) return std::nullopt;
    if (dbLineupLocked(conn.get(), leagueId)) return std::nullopt;
    const auto player = normalizePlayerJson(body["addPlayer"]);
    const auto claimId = timestampId("waiver");
    const auto priority = dbPriorityForManager(conn.get(), leagueId, accountEmail);
    auto orderResult = execParams(conn.get(),
                                  "SELECT COALESCE(MAX(claim_order) + 1, 1) FROM waiver_claims WHERE league_id = $1 AND manager_email = $2 AND status = 'pending'",
                                  {leagueId, accountEmail});
    const auto claimOrder = resultOk(orderResult.get(), PGRES_TUPLES_OK) && PQntuples(orderResult.get()) > 0
                                ? cellInt(orderResult.get(), 0, 0, 1)
                                : 1;
    const auto dropPlayerId = jsonString(body, "dropPlayerId");
    if (dbPlayerRosteredInLeague(conn.get(), leagueId, jsonString(player, "id"))) return std::nullopt;
    if (!dropPlayerId.empty() && !dbManagerHasPlayer(conn.get(), leagueId, accountEmail, dropPlayerId)) return std::nullopt;
    if (dropPlayerId.empty() && !dbRosterHasRoom(conn.get(), leagueId, accountEmail)) return std::nullopt;
    auto result = execParams(conn.get(),
                             "INSERT INTO waiver_claims "
                             "(id, league_id, manager_email, add_player_id, add_player_snapshot, drop_player_id, priority, claim_order) "
                             "VALUES ($1, $2, $3, $4, $5::jsonb, NULLIF($6, ''), $7::int, $8::int)",
                             {claimId, leagueId, accountEmail, jsonString(player, "id"), jsonToString(player), dropPlayerId, std::to_string(priority), std::to_string(claimOrder)});
    if (!resultOk(result.get(), PGRES_COMMAND_OK)) return std::nullopt;
    dbAddTransaction(conn.get(), leagueId, "Waiver Claim", cff::league_waiver::claimTransactionSummary(player), accountEmail, player);
    auto claims = dbListWaivers(accountEmail, leagueId);
    if (claims && claims->size() > 0) {
        return (*claims)[0];
    }
    return std::nullopt;
}

std::optional<Json::Value> dbProcessWaiver(const std::string &accountEmail,
                                           const std::string &leagueId,
                                           const std::string &claimId) {
    if (!dbCanAccessLeague(accountEmail, leagueId)) return std::nullopt;
    auto conn = connectToDb();
    if (!conn) return std::nullopt;
    if (dbLineupLocked(conn.get(), leagueId)) return std::nullopt;
    const auto waiverRules = dbWaiverRules(leagueId).value_or(Json::Value{Json::objectValue});
    if (!cff::league_waiver::deadlinePassed(waiverRules)) return std::nullopt;
    auto claim = execParams(conn.get(),
                            "SELECT add_player_id, add_player_snapshot::text, COALESCE(drop_player_id, '') "
                            "FROM waiver_claims WHERE league_id = $1 AND manager_email = $2 AND id = $3",
                            {leagueId, accountEmail, claimId});
    if (!resultOk(claim.get(), PGRES_TUPLES_OK) || PQntuples(claim.get()) == 0) return std::nullopt;
    const auto player = snapshotPlayer(jsonFromString(cell(claim.get(), 0, 1)), cell(claim.get(), 0, 0));
    const auto dropId = cell(claim.get(), 0, 2);
    const auto addPlayerId = jsonString(player, "id");
    if (dbPlayerRosteredInLeague(conn.get(), leagueId, addPlayerId)
        || (!dropId.empty() && !dbManagerHasPlayer(conn.get(), leagueId, accountEmail, dropId))
        || (dropId.empty() && !dbRosterHasRoom(conn.get(), leagueId, accountEmail))) {
        auto cancel = execParams(conn.get(),
                                 "UPDATE waiver_claims SET status = 'cancelled', processed_at = NOW() "
                                 "WHERE league_id = $1 AND manager_email = $2 AND id = $3",
                                 {leagueId, accountEmail, claimId});
        (void)cancel;
        return std::nullopt;
    }
    if (!dropId.empty()) {
        auto drop = execParams(conn.get(), "DELETE FROM rosters WHERE league_id = $1 AND manager_email = $2 AND player_id = $3", {leagueId, accountEmail, dropId});
        if (!resultOk(drop.get(), PGRES_COMMAND_OK) || std::string{PQcmdTuples(drop.get())} != "1") return std::nullopt;
    }
    const auto slot = dbAssignRosterSlot(conn.get(), leagueId, accountEmail, player);
    if (!slot) return std::nullopt;
    auto rosterInsert = execParams(conn.get(),
                                   "INSERT INTO rosters (league_id, manager_email, player_id, player_snapshot, roster_slot, acquired_via) "
                                   "VALUES ($1, $2, $3, $4::jsonb, $5, 'waiver') "
                                   "ON CONFLICT (league_id, manager_email, player_id) DO UPDATE SET player_snapshot = EXCLUDED.player_snapshot, roster_slot = EXCLUDED.roster_slot",
                                   {leagueId, accountEmail, jsonString(player, "id"), jsonToString(player), *slot});
    auto claimUpdate = execParams(conn.get(),
                                  "UPDATE waiver_claims SET status = 'processed', processed_at = NOW() "
                                  "WHERE league_id = $1 AND manager_email = $2 AND id = $3",
                                  {leagueId, accountEmail, claimId});
    if (!resultOk(rosterInsert.get(), PGRES_COMMAND_OK) || !resultOk(claimUpdate.get(), PGRES_COMMAND_OK)) return std::nullopt;
    dbAddTransaction(conn.get(), leagueId, "Waiver Processed", cff::league_waiver::processedTransactionSummary(player), accountEmail, player);
    Json::Value response;
    response["id"] = claimId;
    response["addPlayer"] = player;
    response["dropPlayerId"] = dropId;
    response["status"] = "Processed";
    return response;
}

std::optional<Json::Value> dbUpdateWaiverStatus(const std::string &accountEmail,
                                                const std::string &leagueId,
                                                const std::string &claimId,
                                                const std::string &status) {
    if (!dbCanAccessLeague(accountEmail, leagueId)) return std::nullopt;
    if (statusForDb(status) != "cancelled") return std::nullopt;
    auto conn = connectToDb();
    if (!conn) return std::nullopt;
    const bool commissioner = dbIsCommissioner(accountEmail, leagueId);
    auto update = execParams(conn.get(),
                             "UPDATE waiver_claims SET status = 'cancelled', processed_at = NOW() "
                             "WHERE league_id = $1 AND id = $2 AND status = 'pending' AND ($4 = 'true' OR manager_email = $3)",
                             {leagueId, claimId, accountEmail, commissioner ? "true" : "false"});
    if (!resultOk(update.get(), PGRES_COMMAND_OK) || std::string{PQcmdTuples(update.get())} == "0") return std::nullopt;
    dbAddTransaction(conn.get(), leagueId, "Waiver Cancelled", cff::league_waiver::cancelledTransactionSummary(), accountEmail, Json::Value{Json::objectValue});
    return dbListWaivers(accountEmail, leagueId);
}

std::optional<Json::Value> dbReorderWaivers(const std::string &accountEmail,
                                            const std::string &leagueId,
                                            const Json::Value &claimIds) {
    if (!dbCanAccessLeague(accountEmail, leagueId) || !claimIds.isArray()) return std::nullopt;
    auto conn = connectToDb();
    if (!conn) return std::nullopt;
    int order = 1;
    for (const auto &claimId : claimIds) {
        if (!claimId.isString() || claimId.asString().empty()) continue;
        auto update = execParams(conn.get(),
                                 "UPDATE waiver_claims SET claim_order = $4::int "
                                 "WHERE league_id = $1 AND manager_email = $2 AND id = $3 AND status = 'pending'",
                                 {leagueId, accountEmail, claimId.asString(), std::to_string(order++)});
        if (!resultOk(update.get(), PGRES_COMMAND_OK)) return std::nullopt;
    }
    return dbListWaivers(accountEmail, leagueId);
}

std::optional<Json::Value> dbProcessWaivers(const std::string &accountEmail, const std::string &leagueId) {
    if (!dbIsCommissioner(accountEmail, leagueId)) return std::nullopt;
    auto conn = connectToDb();
    if (!conn) return std::nullopt;
    if (dbLineupLocked(conn.get(), leagueId)) return std::nullopt;
    const auto waiverRules = dbWaiverRules(leagueId).value_or(Json::Value{Json::objectValue});
    if (!cff::league_waiver::deadlinePassed(waiverRules)) return std::nullopt;
    auto claims = execParams(conn.get(),
                             "SELECT id, manager_email, add_player_id, add_player_snapshot::text, COALESCE(drop_player_id, '') "
                             "FROM waiver_claims WHERE league_id = $1 AND status = 'pending' "
                             "ORDER BY priority ASC, claim_order ASC, created_at ASC",
                             {leagueId});
    if (!resultOk(claims.get(), PGRES_TUPLES_OK)) return std::nullopt;

    Json::Value processed(Json::arrayValue);
    Json::Value cancelled(Json::arrayValue);
    for (int row = 0; row < PQntuples(claims.get()); ++row) {
        const auto claimId = cell(claims.get(), row, 0);
        const auto managerEmail = cell(claims.get(), row, 1);
        const auto addPlayerId = cell(claims.get(), row, 2);
        const auto player = snapshotPlayer(jsonFromString(cell(claims.get(), row, 3)), addPlayerId);
        const auto dropId = cell(claims.get(), row, 4);
        if (dbPlayerRosteredInLeague(conn.get(), leagueId, addPlayerId)
            || (!dropId.empty() && !dbManagerHasPlayer(conn.get(), leagueId, managerEmail, dropId))
            || (dropId.empty() && !dbRosterHasRoom(conn.get(), leagueId, managerEmail))) {
            auto cancel = execParams(conn.get(),
                                     "UPDATE waiver_claims SET status = 'cancelled', processed_at = NOW() WHERE league_id = $1 AND id = $2",
                                     {leagueId, claimId});
            (void)cancel;
            cancelled.append(claimId);
            continue;
        }
        if (!dropId.empty()) {
            auto drop = execParams(conn.get(),
                                   "DELETE FROM rosters WHERE league_id = $1 AND manager_email = $2 AND player_id = $3",
                                   {leagueId, managerEmail, dropId});
            if (!resultOk(drop.get(), PGRES_COMMAND_OK) || std::string{PQcmdTuples(drop.get())} != "1") {
                auto cancel = execParams(conn.get(),
                                         "UPDATE waiver_claims SET status = 'cancelled', processed_at = NOW() WHERE league_id = $1 AND id = $2",
                                         {leagueId, claimId});
                (void)cancel;
                cancelled.append(claimId);
                continue;
            }
        }
        const auto slot = dbAssignRosterSlot(conn.get(), leagueId, managerEmail, player);
        if (!slot) {
            auto cancel = execParams(conn.get(),
                                     "UPDATE waiver_claims SET status = 'cancelled', processed_at = NOW() WHERE league_id = $1 AND id = $2",
                                     {leagueId, claimId});
            (void)cancel;
            cancelled.append(claimId);
            continue;
        }
        auto add = execParams(conn.get(),
                              "INSERT INTO rosters (league_id, manager_email, player_id, player_snapshot, roster_slot, acquired_via) "
                              "VALUES ($1, $2, $3, $4::jsonb, $5, 'waiver') "
                              "ON CONFLICT (league_id, manager_email, player_id) DO UPDATE SET player_snapshot = EXCLUDED.player_snapshot, roster_slot = EXCLUDED.roster_slot, acquired_via = 'waiver'",
                              {leagueId, managerEmail, addPlayerId, jsonToString(player), *slot});
        auto update = execParams(conn.get(),
                                 "UPDATE waiver_claims SET status = 'processed', processed_at = NOW() WHERE league_id = $1 AND id = $2",
                                 {leagueId, claimId});
        if (!resultOk(add.get(), PGRES_COMMAND_OK) || !resultOk(update.get(), PGRES_COMMAND_OK)) {
            return std::nullopt;
        }
        dbMoveManagerToBackOfWaivers(conn.get(), leagueId, managerEmail);
        dbAddTransaction(conn.get(), leagueId, "Waiver Processed", cff::league_waiver::processedTransactionSummary(player), managerEmail, player);
        processed.append(claimId);
    }

    Json::Value payload;
    payload["processed"] = processed;
    payload["cancelled"] = cancelled;
    payload["claims"] = dbListWaivers(accountEmail, leagueId).value_or(Json::Value{Json::arrayValue});
    return payload;
}

std::optional<Json::Value> dbListTrades(const std::string &accountEmail, const std::string &leagueId) {
    if (!dbCanAccessLeague(accountEmail, leagueId)) return std::nullopt;
    auto conn = connectToDb();
    if (!conn) return std::nullopt;
    dbExpireTrades(conn.get(), leagueId);
    const bool commissioner = dbIsCommissioner(accountEmail, leagueId);
    auto result = execParams(conn.get(),
                             "SELECT id, offer_player_snapshot::text, request_player_name, target_manager, status, "
                             "COALESCE(request_player_snapshot::text, '{}'), "
                             "COALESCE(to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), ''), "
                             "offered_by_email, offered_to_email, requires_approval, "
                             "COALESCE(to_char(expires_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), ''), note "
                             "FROM trade_offers WHERE league_id = $1 AND ($3 = 'true' OR offered_by_email = $2 OR offered_to_email = $2) ORDER BY created_at DESC",
                             {leagueId, accountEmail, commissioner ? "true" : "false"});
    if (!resultOk(result.get(), PGRES_TUPLES_OK)) return std::nullopt;
    Json::Value trades(Json::arrayValue);
    for (int row = 0; row < PQntuples(result.get()); ++row) {
        Json::Value trade;
        const auto offer = normalizePlayerJson(jsonFromString(cell(result.get(), row, 1)));
        trade["id"] = cell(result.get(), row, 0);
        trade["offerPlayer"] = offer;
        trade["requestPlayerName"] = cell(result.get(), row, 2);
        trade["targetManager"] = cell(result.get(), row, 3);
        trade["status"] = statusForUi(cell(result.get(), row, 4));
        trade["requestPlayer"] = normalizePlayerJson(jsonFromString(cell(result.get(), row, 5)));
        trade["createdAt"] = cell(result.get(), row, 6);
        trade["offeredByEmail"] = cell(result.get(), row, 7);
        trade["offeredToEmail"] = cell(result.get(), row, 8);
        trade["requiresApproval"] = cellBool(result.get(), row, 9);
        trade["expiresAt"] = cell(result.get(), row, 10);
        trade["note"] = cell(result.get(), row, 11);
        trades.append(trade);
    }
    return trades;
}

std::optional<Json::Value> dbCreateTrade(const std::string &accountEmail,
                                         const std::string &leagueId,
                                         const Json::Value &body) {
    if (!dbCanAccessLeague(accountEmail, leagueId)) return std::nullopt;
    auto conn = connectToDb();
    if (!conn) return std::nullopt;
    if (dbLineupLocked(conn.get(), leagueId)) return std::nullopt;
    const auto tradeId = timestampId("trade");
    const auto target = jsonString(body, "targetManager", jsonString(body, "targetManagerEmail"));
    const auto offerId = jsonString(body["offerPlayer"], "id");
    const auto requestedId = body.isMember("requestPlayer") && body["requestPlayer"].isObject()
                                 ? jsonString(body["requestPlayer"], "id")
                                 : "";
    const auto rules = dbTradeRules(leagueId).value_or(Json::Value{Json::objectValue});
    const auto requiresApproval = cff::league_trade::approvalRequired(rules);
    const auto expirationHours = cff::league_trade::expirationHours(rules);
    if (!cff::league_trade::validTarget(accountEmail, target)) return std::nullopt;
    if (!dbIsActiveMember(conn.get(), leagueId, accountEmail) || !dbIsActiveMember(conn.get(), leagueId, target)) return std::nullopt;
    auto offer = dbRosterPlayer(conn.get(), leagueId, accountEmail, offerId);
    auto requestPlayer = dbRosterPlayer(conn.get(), leagueId, target, requestedId);
    if (!offer || !requestPlayer) return std::nullopt;
    if (dbPlayerLockedInTrade(conn.get(), leagueId, accountEmail, offerId)) return std::nullopt;
    if (dbPlayerLockedInTrade(conn.get(), leagueId, target, requestedId)) return std::nullopt;
    auto result = execParams(conn.get(),
                             "INSERT INTO trade_offers "
                             "(id, league_id, offered_by_email, offered_to_email, offered_player_ids, "
                             "requested_player_ids, offer_player_snapshot, request_player_snapshot, request_player_name, target_manager, note, requires_approval, expires_at) "
                             "VALUES ($1, $2, $3, $4, ARRAY[$5], ARRAY[$6], $7::jsonb, $8::jsonb, $9, $10, $11, $12::boolean, NOW() + ($13::int * INTERVAL '1 hour'))",
                             {tradeId, leagueId, accountEmail, target,
                              offerId, requestedId,
                              jsonToString(*offer), jsonToString(*requestPlayer), jsonString(body, "requestPlayerName"), target,
                              jsonString(body, "note"), requiresApproval ? "true" : "false", std::to_string(expirationHours)});
    if (!resultOk(result.get(), PGRES_COMMAND_OK)) return std::nullopt;
    dbAddTransaction(conn.get(), leagueId, "Trade Offer", cff::league_trade::offerTransactionSummary(*offer), accountEmail, *offer);
    auto trades = dbListTrades(accountEmail, leagueId);
    if (trades && trades->size() > 0) {
        return (*trades)[0];
    }
    return std::nullopt;
}

std::optional<Json::Value> dbUpdateTradeStatus(const std::string &accountEmail,
                                               const std::string &leagueId,
                                               const std::string &tradeId,
                                               const std::string &status) {
    if (!dbCanAccessLeague(accountEmail, leagueId)) return std::nullopt;
    auto conn = connectToDb();
    if (!conn) return std::nullopt;
    dbExpireTrades(conn.get(), leagueId);
    const bool commissioner = dbIsCommissioner(accountEmail, leagueId);
    auto result = execParams(conn.get(),
                             "SELECT offered_by_email, offered_to_email, offer_player_snapshot::text, request_player_snapshot::text, "
                             "request_player_name, target_manager, requires_approval, status, note "
                             "FROM trade_offers WHERE league_id = $1 AND id = $2",
                             {leagueId, tradeId});
    if (!resultOk(result.get(), PGRES_TUPLES_OK) || PQntuples(result.get()) == 0) return std::nullopt;
    const auto offeredBy = cell(result.get(), 0, 0);
    const auto offeredTo = cell(result.get(), 0, 1);
    const auto offer = normalizePlayerJson(jsonFromString(cell(result.get(), 0, 2)));
    const auto requestPlayer = normalizePlayerJson(jsonFromString(cell(result.get(), 0, 3)));
    const auto currentStatus = cell(result.get(), 0, 7);
    if (!cff::league_trade::openStatus(currentStatus)) return std::nullopt;
    const bool involved = accountEmail == offeredBy || accountEmail == offeredTo;
    const bool requiresApproval = cellBool(result.get(), 0, 6);
    const auto decision = cff::league_trade::decideStatus(
        status, requiresApproval, involved, commissioner, true);
    if (!decision.allowed) return std::nullopt;
    const auto &nextStatus = decision.databaseStatus;
    const bool executeTrade = decision.execute;
    if (executeTrade) {
        if (dbLineupLocked(conn.get(), leagueId)) return std::nullopt;
        if (jsonString(requestPlayer, "id").empty()) {
            return std::nullopt;
        }
        if (!dbRosterPlayer(conn.get(), leagueId, offeredBy, jsonString(offer, "id"))
            || !dbRosterPlayer(conn.get(), leagueId, offeredTo, jsonString(requestPlayer, "id"))) {
            return std::nullopt;
        }
        auto removeOffer = execParams(conn.get(),
                                      "DELETE FROM rosters WHERE league_id = $1 AND manager_email = $2 AND player_id = $3",
                                      {leagueId, offeredBy, jsonString(offer, "id")});
        auto removeRequest = execParams(conn.get(),
                                        "DELETE FROM rosters WHERE league_id = $1 AND manager_email = $2 AND player_id = $3",
                                        {leagueId, offeredTo, jsonString(requestPlayer, "id")});
        const auto offerSlot = dbAssignRosterSlot(conn.get(), leagueId, offeredTo, offer);
        const auto requestSlot = dbAssignRosterSlot(conn.get(), leagueId, offeredBy, requestPlayer);
        if (!offerSlot || !requestSlot) {
            return std::nullopt;
        }
        auto addOffer = execParams(conn.get(),
                                   "INSERT INTO rosters (league_id, manager_email, player_id, player_snapshot, roster_slot, acquired_via) "
                                   "VALUES ($1, $2, $3, $4::jsonb, $5, 'trade') "
                                   "ON CONFLICT (league_id, manager_email, player_id) DO UPDATE SET player_snapshot = EXCLUDED.player_snapshot, roster_slot = EXCLUDED.roster_slot, acquired_via = 'trade'",
                                   {leagueId, offeredTo, jsonString(offer, "id"), jsonToString(offer), *offerSlot});
        auto addRequest = execParams(conn.get(),
                                     "INSERT INTO rosters (league_id, manager_email, player_id, player_snapshot, roster_slot, acquired_via) "
                                     "VALUES ($1, $2, $3, $4::jsonb, $5, 'trade') "
                                     "ON CONFLICT (league_id, manager_email, player_id) DO UPDATE SET player_snapshot = EXCLUDED.player_snapshot, roster_slot = EXCLUDED.roster_slot, acquired_via = 'trade'",
                                     {leagueId, offeredBy, jsonString(requestPlayer, "id"), jsonToString(requestPlayer), *requestSlot});
        if (!resultOk(removeOffer.get(), PGRES_COMMAND_OK) || !resultOk(removeRequest.get(), PGRES_COMMAND_OK)
            || std::string{PQcmdTuples(removeOffer.get())} != "1" || std::string{PQcmdTuples(removeRequest.get())} != "1"
            || !resultOk(addOffer.get(), PGRES_COMMAND_OK) || !resultOk(addRequest.get(), PGRES_COMMAND_OK)) {
            return std::nullopt;
        }
    }
    auto update = execParams(conn.get(),
                             "UPDATE trade_offers SET status = $3, resolved_at = CASE WHEN $3 IN ('approved', 'declined', 'vetoed', 'cancelled') THEN NOW() ELSE resolved_at END "
                             "WHERE league_id = $1 AND id = $2 AND status IN ('pending', 'accepted')",
                             {leagueId, tradeId, nextStatus});
    if (!resultOk(update.get(), PGRES_COMMAND_OK)) return std::nullopt;
    dbAddTransaction(conn.get(), leagueId, "Trade", cff::league_trade::statusTransactionSummary(decision.displayStatus, offer), accountEmail, offer);
    Json::Value trade;
    trade["id"] = tradeId;
    trade["offerPlayer"] = offer;
    trade["requestPlayer"] = requestPlayer;
    trade["requestPlayerName"] = cell(result.get(), 0, 4);
    trade["targetManager"] = cell(result.get(), 0, 5);
    trade["offeredByEmail"] = offeredBy;
    trade["offeredToEmail"] = offeredTo;
    trade["requiresApproval"] = requiresApproval;
    trade["status"] = decision.displayStatus;
    trade["note"] = cell(result.get(), 0, 8);
    return trade;
}

std::optional<Json::Value> dbListTransactions(const std::string &accountEmail, const std::string &leagueId) {
    if (!dbCanAccessLeague(accountEmail, leagueId)) return std::nullopt;
    auto conn = connectToDb();
    if (!conn) return std::nullopt;
    auto result = execParams(conn.get(),
                             "SELECT id, transaction_type, summary, COALESCE(manager_email, ''), "
                             "COALESCE(to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), '') "
                             "FROM transactions WHERE league_id = $1 ORDER BY created_at DESC LIMIT 100",
                             {leagueId});
    if (!resultOk(result.get(), PGRES_TUPLES_OK)) return std::nullopt;
    Json::Value txns(Json::arrayValue);
    for (int row = 0; row < PQntuples(result.get()); ++row) {
        Json::Value txn;
        txn["id"] = cell(result.get(), row, 0);
        txn["type"] = cell(result.get(), row, 1);
        txn["summary"] = cell(result.get(), row, 2);
        txn["managerEmail"] = cell(result.get(), row, 3);
        txn["createdAt"] = cell(result.get(), row, 4);
        txns.append(txn);
    }
    return txns;
}

std::optional<Json::Value> dbCreateLeagueFeedPost(const std::string &accountEmail,
                                                  const std::string &leagueId,
                                                  const std::string &body) {
    if (!dbIsCommissioner(accountEmail, leagueId)) return std::nullopt;
    auto conn = connectToDb();
    if (!conn) return std::nullopt;
    const auto trimmed = body.substr(0, std::min<std::size_t>(body.size(), 2000));
    if (trimmed.empty()) return std::nullopt;
    const auto postId = timestampId("feed-post");
    auto result = execParams(conn.get(),
                             "INSERT INTO league_feed_posts (id, league_id, manager_email, post_type, body) "
                             "VALUES ($1, $2, $3, 'commissioner_post', $4) "
                             "RETURNING id, post_type, body, manager_email, "
                             "COALESCE(to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), '')",
                             {postId, leagueId, accountEmail, trimmed});
    if (!resultOk(result.get(), PGRES_TUPLES_OK) || PQntuples(result.get()) == 0) return std::nullopt;
    return feedItem("Commissioner Post", cell(result.get(), 0, 2), cell(result.get(), 0, 4), cell(result.get(), 0, 3), "Post");
}

std::optional<Json::Value> dbListLeagueFeed(const std::string &accountEmail, const std::string &leagueId) {
    if (!dbCanAccessLeague(accountEmail, leagueId)) return std::nullopt;
    auto conn = connectToDb();
    if (!conn) return std::nullopt;

    Json::Value items(Json::arrayValue);

    auto posts = execParams(conn.get(),
                            "SELECT post_type, body, manager_email, "
                            "COALESCE(to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), '') "
                            "FROM league_feed_posts WHERE league_id = $1 ORDER BY created_at DESC LIMIT 100",
                            {leagueId});
    if (!resultOk(posts.get(), PGRES_TUPLES_OK)) return std::nullopt;
    for (int row = 0; row < PQntuples(posts.get()); ++row) {
        const auto type = cell(posts.get(), row, 0) == "commissioner_post" ? "Commissioner Post" : cell(posts.get(), row, 0);
        items.append(feedItem(type, cell(posts.get(), row, 1), cell(posts.get(), row, 3), cell(posts.get(), row, 2), "Post"));
    }

    auto txns = execParams(conn.get(),
                           "SELECT transaction_type, summary, COALESCE(manager_email, ''), "
                           "COALESCE(to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), '') "
                           "FROM transactions WHERE league_id = $1 ORDER BY created_at DESC LIMIT 100",
                           {leagueId});
    if (!resultOk(txns.get(), PGRES_TUPLES_OK)) return std::nullopt;
    for (int row = 0; row < PQntuples(txns.get()); ++row) {
        items.append(feedItem(cell(txns.get(), row, 0), cell(txns.get(), row, 1), cell(txns.get(), row, 3), cell(txns.get(), row, 2), "Transaction"));
    }

    auto waivers = execParams(conn.get(),
                              "SELECT status, add_player_snapshot::text, COALESCE(drop_player_id, ''), manager_email, "
                              "COALESCE(to_char(COALESCE(processed_at, created_at) AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), '') "
                              "FROM waiver_claims WHERE league_id = $1 ORDER BY COALESCE(processed_at, created_at) DESC LIMIT 100",
                              {leagueId});
    if (!resultOk(waivers.get(), PGRES_TUPLES_OK)) return std::nullopt;
    for (int row = 0; row < PQntuples(waivers.get()); ++row) {
        const auto status = statusForUi(cell(waivers.get(), row, 0));
        const auto player = jsonFromString(cell(waivers.get(), row, 1));
        items.append(feedItem("Waiver " + status,
                              status + ": " + jsonString(player, "name", "player") + (cell(waivers.get(), row, 2).empty() ? "" : " with a drop"),
                              cell(waivers.get(), row, 4),
                              cell(waivers.get(), row, 3),
                              "Waiver"));
    }

    auto trades = execParams(conn.get(),
                             "SELECT status, offer_player_snapshot::text, request_player_snapshot::text, request_player_name, offered_by_email, "
                             "COALESCE(to_char(COALESCE(resolved_at, created_at) AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), '') "
                             "FROM trade_offers WHERE league_id = $1 ORDER BY COALESCE(resolved_at, created_at) DESC LIMIT 100",
                             {leagueId});
    if (!resultOk(trades.get(), PGRES_TUPLES_OK)) return std::nullopt;
    for (int row = 0; row < PQntuples(trades.get()); ++row) {
        const auto status = statusForUi(cell(trades.get(), row, 0));
        const auto offer = jsonFromString(cell(trades.get(), row, 1));
        const auto request = jsonFromString(cell(trades.get(), row, 2));
        const auto requestName = jsonString(request, "name", cell(trades.get(), row, 3).empty() ? "return" : cell(trades.get(), row, 3));
        items.append(feedItem("Trade " + status,
                              status + ": " + jsonString(offer, "name", "player") + " for " + requestName,
                              cell(trades.get(), row, 5),
                              cell(trades.get(), row, 4),
                              "Trade"));
    }

    auto matchups = execParams(conn.get(),
                               "SELECT week, home_manager_email, COALESCE(away_manager_email, ''), home_score, away_score, "
                               "COALESCE(to_char(COALESCE(finalized_at, updated_at) AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), '') "
                               "FROM league_matchups WHERE league_id = $1 AND status = 'final' ORDER BY COALESCE(finalized_at, updated_at) DESC LIMIT 100",
                               {leagueId});
    if (!resultOk(matchups.get(), PGRES_TUPLES_OK)) return std::nullopt;
    double highScore = -1.0;
    double lowScore = 1000000.0;
    double largestMargin = -1.0;
    std::string highManager;
    std::string lowManager;
    std::string marginWinner;
    std::string marginLoser;
    std::string awardsAt;
    for (int row = 0; row < PQntuples(matchups.get()); ++row) {
        const auto home = cell(matchups.get(), row, 1);
        const auto away = cell(matchups.get(), row, 2);
        const auto homeScore = std::stod(cell(matchups.get(), row, 3).empty() ? "0" : cell(matchups.get(), row, 3));
        const auto awayScore = std::stod(cell(matchups.get(), row, 4).empty() ? "0" : cell(matchups.get(), row, 4));
        const auto winner = homeScore >= awayScore ? home : away;
        const auto loser = homeScore >= awayScore ? away : home;
        const auto winnerScore = std::max(homeScore, awayScore);
        const auto loserScore = std::min(homeScore, awayScore);
        std::ostringstream finalSummary;
        finalSummary << winner << " beat " << loser << " " << std::fixed << std::setprecision(1) << winnerScore << "-" << loserScore << ".";
        items.append(feedItem("Final Score", finalSummary.str(), cell(matchups.get(), row, 5), winner, "Final"));
        awardsAt = awardsAt.empty() ? cell(matchups.get(), row, 5) : awardsAt;
        if (homeScore > highScore) { highScore = homeScore; highManager = home; }
        if (awayScore > highScore) { highScore = awayScore; highManager = away; }
        if (homeScore < lowScore) { lowScore = homeScore; lowManager = home; }
        if (awayScore < lowScore) { lowScore = awayScore; lowManager = away; }
        if (winnerScore - loserScore > largestMargin) {
            largestMargin = winnerScore - loserScore;
            marginWinner = winner;
            marginLoser = loser;
        }
    }
    if (!highManager.empty()) {
        std::ostringstream highSummary;
        highSummary << highManager << " posted the high score with " << std::fixed << std::setprecision(1) << highScore << " points.";
        items.append(feedItem("Weekly Award", highSummary.str(), awardsAt, highManager, "Highest Score"));
    }
    if (!lowManager.empty()) {
        std::ostringstream lowSummary;
        lowSummary << lowManager << " survived the lowest score at " << std::fixed << std::setprecision(1) << lowScore << " points.";
        items.append(feedItem("Weekly Award", lowSummary.str(), awardsAt, lowManager, "Lowest Score"));
    }
    if (!marginWinner.empty() && largestMargin > 0) {
        std::ostringstream marginSummary;
        marginSummary << marginWinner << " won by " << std::fixed << std::setprecision(1) << largestMargin << " points over " << marginLoser << ".";
        items.append(feedItem("Weekly Award", marginSummary.str(), awardsAt, marginWinner, "Largest Margin"));
    }

    sortFeedItems(items);
    return items;
}

double dbProjectedScore(PGconn *conn, const std::string &leagueId, const std::string &managerEmail) {
    auto result = execParams(conn,
                             "SELECT player_snapshot::text, roster_slot FROM rosters WHERE league_id = $1 AND manager_email = $2",
                             {leagueId, managerEmail});
    if (!resultOk(result.get(), PGRES_TUPLES_OK)) return 0.0;
    double total = 0.0;
    for (int row = 0; row < PQntuples(result.get()); ++row) {
        if (lowerString(cell(result.get(), row, 1)) == "bench") {
            continue;
        }
        total += projectionForPlayer(jsonFromString(cell(result.get(), row, 0)));
    }
    return total;
}

Json::Value matchupsFromResult(PGresult *result) {
    Json::Value matchups(Json::arrayValue);
    for (int row = 0; row < PQntuples(result); ++row) {
        Json::Value matchup;
        matchup["id"] = cell(result, row, 0);
        matchup["week"] = cellInt(result, row, 1, 1);
        matchup["homeManager"] = cell(result, row, 2);
        matchup["awayManager"] = cell(result, row, 3);
        matchup["homeScore"] = std::stod(cell(result, row, 4).empty() ? "0" : cell(result, row, 4));
        matchup["awayScore"] = std::stod(cell(result, row, 5).empty() ? "0" : cell(result, row, 5));
        matchup["status"] = cell(result, row, 6);
        matchup["createdAt"] = cell(result, row, 7);
        if (PQnfields(result) > 8) {
            matchup["finalizedAt"] = cell(result, row, 8);
        }
        matchups.append(matchup);
    }
    return matchups;
}

bool dbWeekFinalized(PGconn *conn, const std::string &leagueId, int week) {
    auto result = execParams(conn,
                             "SELECT 1 FROM league_matchups WHERE league_id = $1 AND week = $2::int AND status = 'final' LIMIT 1",
                             {leagueId, std::to_string(week)});
    return resultOk(result.get(), PGRES_TUPLES_OK) && PQntuples(result.get()) > 0;
}

std::optional<Json::Value> dbSaveMatchups(PGconn *conn,
                                          const std::string &leagueId,
                                          int week,
                                          const Json::Value &matchups,
                                          const std::string &status = "scheduled") {
    if (dbWeekFinalized(conn, leagueId, week)) return std::nullopt;
    auto clear = execParams(conn, "DELETE FROM league_matchups WHERE league_id = $1 AND week = $2::int", {leagueId, std::to_string(week)});
    if (!resultOk(clear.get(), PGRES_COMMAND_OK)) return std::nullopt;
    for (const auto &matchup : matchups) {
        auto insert = execParams(conn,
                                 "INSERT INTO league_matchups (id, league_id, week, home_manager_email, away_manager_email, home_score, away_score, status) "
                                 "VALUES ($1, $2, $3::int, $4, NULLIF($5, ''), $6::numeric, $7::numeric, $8)",
                                 {jsonString(matchup, "id"), leagueId, std::to_string(week),
                                  jsonString(matchup, "homeManager"), jsonString(matchup, "awayManager"),
                                  std::to_string(matchup["homeScore"].asDouble()), std::to_string(matchup["awayScore"].asDouble()),
                                  status});
        if (!resultOk(insert.get(), PGRES_COMMAND_OK)) return std::nullopt;
    }
    return matchups;
}

std::optional<Json::Value> dbGenerateMatchups(const std::string &accountEmail, const std::string &leagueId, int week = 1) {
    if (!dbCanAccessLeague(accountEmail, leagueId)) return std::nullopt;
    auto conn = connectToDb();
    if (!conn) return std::nullopt;
    if (dbWeekFinalized(conn.get(), leagueId, week)) return std::nullopt;
    auto members = membersForLeague(conn.get(), leagueId);
    auto matchups = cff::league_schedule::buildMatchups(members, leagueId, week, [conn = conn.get(), &leagueId](const std::string &managerEmail) {
        return dbProjectedScore(conn, leagueId, managerEmail);
    });
    int matchupIndex = 1;
    for (auto &matchup : matchups) {
        matchup["week"] = week;
        matchup["id"] = leagueId + "-week-" + std::to_string(week) + "-" + std::to_string(matchupIndex++);
    }
    if (!dbSaveMatchups(conn.get(), leagueId, week, matchups)) return std::nullopt;
    dbAddTransaction(conn.get(), leagueId, "Schedule", "Generated week " + std::to_string(week) + " matchups", accountEmail, Json::Value{Json::objectValue});
    return matchups;
}

std::optional<Json::Value> dbGenerateSeasonSchedule(const std::string &accountEmail,
                                                    const std::string &leagueId,
                                                    int weeks) {
    if (!dbIsCommissioner(accountEmail, leagueId)) return std::nullopt;
    auto conn = connectToDb();
    if (!conn) return std::nullopt;
    auto finalized = execParams(conn.get(),
                                "SELECT 1 FROM league_matchups WHERE league_id = $1 AND status = 'final' LIMIT 1",
                                {leagueId});
    if (resultOk(finalized.get(), PGRES_TUPLES_OK) && PQntuples(finalized.get()) > 0) {
        return std::nullopt;
    }
    auto members = membersForLeague(conn.get(), leagueId);
    auto schedule = cff::league_schedule::buildSeasonSchedule(members, leagueId, weeks, [conn = conn.get(), &leagueId](const std::string &managerEmail) {
        return dbProjectedScore(conn, leagueId, managerEmail);
    });
    auto clear = execParams(conn.get(), "DELETE FROM league_matchups WHERE league_id = $1", {leagueId});
    if (!resultOk(clear.get(), PGRES_COMMAND_OK)) return std::nullopt;
    for (int week = 1; week <= weeks; ++week) {
        Json::Value weekly(Json::arrayValue);
        for (const auto &matchup : schedule) {
            if (cff::getIntOrDefault(matchup, "week", 1) == week) {
                weekly.append(matchup);
            }
        }
        if (!dbSaveMatchups(conn.get(), leagueId, week, weekly)) return std::nullopt;
    }
    dbAddTransaction(conn.get(), leagueId, "Schedule", "Generated " + std::to_string(weeks) + "-week season schedule", accountEmail, Json::Value{Json::objectValue});
    return schedule;
}

std::optional<Json::Value> dbListMatchups(const std::string &accountEmail, const std::string &leagueId) {
    if (!dbCanAccessLeague(accountEmail, leagueId)) return std::nullopt;
    auto conn = connectToDb();
    if (!conn) return std::nullopt;
    auto result = execParams(conn.get(),
                             "SELECT id, week, home_manager_email, COALESCE(away_manager_email, ''), home_score, away_score, status, "
                             "COALESCE(to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), ''), "
                             "COALESCE(to_char(finalized_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), '') "
                             "FROM league_matchups WHERE league_id = $1 ORDER BY week, created_at",
                             {leagueId});
    if (!resultOk(result.get(), PGRES_TUPLES_OK)) return std::nullopt;
    if (PQntuples(result.get()) == 0) {
        return dbGenerateMatchups(accountEmail, leagueId);
    }
    return matchupsFromResult(result.get());
}

std::optional<Json::Value> dbLineupErrors(PGconn *conn, const std::string &leagueId) {
    auto rules = dbRosterRules(leagueId).value_or(Json::Value{Json::objectValue});
    auto members = membersForLeague(conn, leagueId);
    Json::Value errors(Json::arrayValue);
    for (const auto &member : members) {
        if (jsonString(member, "status") == "Removed") continue;
        const auto managerEmail = jsonString(member, "email");
        auto result = execParams(conn,
                                 "SELECT roster_slot, COUNT(*) FROM rosters WHERE league_id = $1 AND manager_email = $2 AND LOWER(roster_slot) <> 'bench' GROUP BY roster_slot",
                                 {leagueId, managerEmail});
        if (!resultOk(result.get(), PGRES_TUPLES_OK)) return std::nullopt;
        std::unordered_map<std::string, int> counts;
        for (int row = 0; row < PQntuples(result.get()); ++row) {
            counts[lowerString(cell(result.get(), row, 0))] = cellInt(result.get(), row, 1, 0);
        }
        const auto managerErrors = cff::league_roster::lineupErrorsFromCounts(managerEmail, rules, counts);
        for (const auto &error : managerErrors) {
            errors.append(error);
        }
    }
    return errors;
}

std::optional<Json::Value> dbScoreWeek(const std::string &accountEmail,
                                       const std::string &leagueId,
                                       int season,
                                       int week) {
    if (!dbIsCommissioner(accountEmail, leagueId)) return std::nullopt;
    auto conn = connectToDb();
    if (!conn) return std::nullopt;
    if (dbWeekFinalized(conn.get(), leagueId, week)) return std::nullopt;
    auto lineupErrors = dbLineupErrors(conn.get(), leagueId);
    if (!lineupErrors || lineupErrors->size() > 0) {
        Json::Value payload;
        payload["error"] = "Invalid lineup";
        payload["lineupErrors"] = lineupErrors.value_or(Json::Value{Json::arrayValue});
        return payload;
    }
    auto settingsResult = execParams(conn.get(),
                                     "SELECT scoring_settings::text FROM leagues WHERE id = $1",
                                     {leagueId});
    if (!resultOk(settingsResult.get(), PGRES_TUPLES_OK) || PQntuples(settingsResult.get()) == 0) {
        return std::nullopt;
    }
    const auto settings = jsonFromString(cell(settingsResult.get(), 0, 0));
    auto statsResult = execParams(conn.get(),
                                  "SELECT r.manager_email, r.player_id, r.player_snapshot::text, "
                                  "COALESCE(ps.category, ''), COALESCE(ps.stat_name, ''), COALESCE(ps.stat_value, 0) "
                                  "FROM rosters r "
                                  "LEFT JOIN player_stats ps ON ps.player_id = r.player_id AND ps.season = $2::int AND ps.week = $3::int "
                                  "WHERE r.league_id = $1 AND LOWER(r.roster_slot) <> 'bench' "
                                  "ORDER BY r.manager_email, r.player_id",
                                  {leagueId, std::to_string(season), std::to_string(week)});
    if (!resultOk(statsResult.get(), PGRES_TUPLES_OK)) return std::nullopt;

    std::unordered_map<std::string, double> playerTotals;
    std::unordered_map<std::string, double> managerTotals;
    std::unordered_map<std::string, std::string> playerManagers;
    std::unordered_map<std::string, std::string> playerIds;
    std::unordered_map<std::string, Json::Value> playerStats;

    for (int row = 0; row < PQntuples(statsResult.get()); ++row) {
        const auto manager = cell(statsResult.get(), row, 0);
        const auto playerId = cell(statsResult.get(), row, 1);
        const auto key = manager + "\n" + playerId;
        playerManagers[key] = manager;
        playerIds[key] = playerId;
        if (!playerStats[key].isObject()) {
            playerStats[key] = Json::Value{Json::objectValue};
        }
        const auto category = cell(statsResult.get(), row, 3);
        const auto statName = cell(statsResult.get(), row, 4);
        const auto rawValue = cell(statsResult.get(), row, 5);
        const auto value = rawValue.empty() ? 0.0 : std::stod(rawValue);
        if (!category.empty() && !statName.empty()) {
            playerStats[key][category + "." + statName] = value;
            const auto points = fantasyPointsForStat(settings, category, statName, value);
            playerTotals[key] += points;
            managerTotals[manager] += points;
        }
    }

    Json::Value scores(Json::arrayValue);
    for (const auto &entry : playerManagers) {
        const auto &key = entry.first;
        const auto &manager = entry.second;
        const auto &playerId = playerIds[key];
        const auto points = playerTotals[key];
        auto upsert = execParams(conn.get(),
                                 "INSERT INTO fantasy_player_scores (league_id, manager_email, player_id, season, week, fantasy_points, stats, updated_at) "
                                 "VALUES ($1, $2, $3, $4::int, $5::int, $6::numeric, $7::jsonb, NOW()) "
                                 "ON CONFLICT (league_id, manager_email, player_id, season, week) "
                                 "DO UPDATE SET fantasy_points = EXCLUDED.fantasy_points, stats = EXCLUDED.stats, updated_at = NOW()",
                                 {leagueId, manager, playerId, std::to_string(season), std::to_string(week),
                                  std::to_string(points), jsonToString(playerStats[key])});
        if (!resultOk(upsert.get(), PGRES_COMMAND_OK)) return std::nullopt;
        Json::Value score;
        score["managerEmail"] = manager;
        score["playerId"] = playerId;
        score["fantasyPoints"] = points;
        score["stats"] = playerStats[key];
        scores.append(score);
    }

    auto members = membersForLeague(conn.get(), leagueId);
    auto matchups = cff::league_schedule::buildMatchups(members, leagueId, week, [&managerTotals](const std::string &managerEmail) {
        return managerTotals[managerEmail];
    });
    int matchupIndex = 1;
    for (auto &matchup : matchups) {
        matchup["week"] = week;
        matchup["id"] = leagueId + "-week-" + std::to_string(week) + "-" + std::to_string(matchupIndex++);
    }
    if (!dbSaveMatchups(conn.get(), leagueId, week, matchups, "scheduled")) return std::nullopt;
    dbAddTransaction(conn.get(), leagueId, "Scoring", "Calculated week " + std::to_string(week) + " fantasy scores", accountEmail, Json::Value{Json::objectValue});

    Json::Value payload;
    payload["season"] = season;
    payload["week"] = week;
    payload["scores"] = scores;
    payload["matchups"] = matchups;
    return payload;
}

std::optional<Json::Value> dbFinalizeWeek(const std::string &accountEmail,
                                          const std::string &leagueId,
                                          int week) {
    if (!dbIsCommissioner(accountEmail, leagueId)) return std::nullopt;
    auto conn = connectToDb();
    if (!conn) return std::nullopt;
    auto lineupErrors = dbLineupErrors(conn.get(), leagueId);
    if (!lineupErrors || lineupErrors->size() > 0) {
        Json::Value payload;
        payload["error"] = "Invalid lineup";
        payload["lineupErrors"] = lineupErrors.value_or(Json::Value{Json::arrayValue});
        return payload;
    }
    auto existing = execParams(conn.get(),
                               "SELECT COUNT(*) FROM league_matchups WHERE league_id = $1 AND week = $2::int",
                               {leagueId, std::to_string(week)});
    if (!resultOk(existing.get(), PGRES_TUPLES_OK) || cellInt(existing.get(), 0, 0, 0) == 0) {
        return std::nullopt;
    }
    auto update = execParams(conn.get(),
                             "UPDATE league_matchups SET status = 'final', finalized_at = COALESCE(finalized_at, NOW()), updated_at = NOW() "
                             "WHERE league_id = $1 AND week = $2::int",
                             {leagueId, std::to_string(week)});
    if (!resultOk(update.get(), PGRES_COMMAND_OK)) return std::nullopt;
    dbAddTransaction(conn.get(), leagueId, "Scoring Finalized", "Finalized week " + std::to_string(week), accountEmail, Json::Value{Json::objectValue});
    auto result = execParams(conn.get(),
                             "SELECT id, week, home_manager_email, COALESCE(away_manager_email, ''), home_score, away_score, status, "
                             "COALESCE(to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), ''), "
                             "COALESCE(to_char(finalized_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), '') "
                             "FROM league_matchups WHERE league_id = $1 AND week = $2::int ORDER BY created_at",
                             {leagueId, std::to_string(week)});
    if (!resultOk(result.get(), PGRES_TUPLES_OK)) return std::nullopt;
    return matchupsFromResult(result.get());
}
#endif
} // namespace

void handleCreateLeague(const drogon::HttpRequestPtr &req,
                        std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                        const std::string &accountEmail) {
    const auto body = req->getJsonObject();
    const auto normalized = normalizeLeaguePayload(body ? *body : Json::Value{});
    auto league = cff::League::fromJson(normalized);
    if (!draftDateAtTopOfHour(league.draftDate)) {
        sendError(callback, drogon::k400BadRequest, "Draft time must be scheduled at the top of an hour");
        return;
    }

#ifdef CFF_HAS_POSTGRES
    if (dbConfigured()) {
        auto created = dbCreateLeague(accountEmail, league);
        if (!created) {
            sendError(callback, drogon::k409Conflict, "Unable to create league. Check the three-league account limit and database schema.");
            return;
        }
        (*created)["message"] = "League created";
        callback(jsonResponse(*created, drogon::k201Created));
        return;
    }
#endif

    std::lock_guard<std::mutex> lock(storeMutex);
    auto &ownerLeagues = leagueIdsByOwner[accountEmail];
    if (ownerLeagues.size() >= kMaxLeaguesPerAccount) {
        sendError(callback, drogon::k409Conflict, "Each account can have up to three leagues");
        return;
    }

    leaguesById[league.id] = LeagueRecord{league, accountEmail};
    ownerLeagues.push_back(league.id);
    auto &members = arrayForLeague(membersByLeague, league.id);
    Json::Value owner;
    owner["email"] = accountEmail;
    owner["role"] = "commissioner";
    owner["status"] = "Active";
    owner["teamName"] = "";
    members.append(owner);
    for (const auto &email : league.invitedEmails) {
        Json::Value member;
        member["email"] = email;
        member["role"] = "member";
        member["status"] = "Invited";
        members.append(member);
    }
    Json::Value payload = league.toJson();
    payload["members"] = members;
    payload["message"] = "League created";
    callback(jsonResponse(payload, drogon::k201Created));
}

void handleListLeagues(const drogon::HttpRequestPtr&,
                       std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                       const std::string &accountEmail) {
#ifdef CFF_HAS_POSTGRES
    if (dbConfigured()) {
        auto leagues = dbListLeagues(accountEmail);
        if (!leagues) {
            sendError(callback, drogon::k500InternalServerError, "Unable to list leagues");
            return;
        }
        callback(jsonResponse(*leagues, drogon::k200OK));
        return;
    }
#endif

    std::lock_guard<std::mutex> lock(storeMutex);
    Json::Value payload(Json::arrayValue);
    for (const auto &leagueId : leagueIdsByOwner[accountEmail]) {
        const auto it = leaguesById.find(leagueId);
        if (it != leaguesById.end()) {
            auto league = it->second.league.toJson();
            league["members"] = arrayForLeague(membersByLeague, leagueId);
            payload.append(league);
        }
    }
    callback(jsonResponse(payload, drogon::k200OK));
}

void handleGetLeague(const drogon::HttpRequestPtr&,
                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                     const std::string &accountEmail,
                     const std::string &leagueId) {
#ifdef CFF_HAS_POSTGRES
    if (dbConfigured()) {
        auto league = dbGetLeague(accountEmail, leagueId);
        if (!league) {
            sendError(callback, drogon::k404NotFound, "League not found");
            return;
        }
        callback(jsonResponse(*league, drogon::k200OK));
        return;
    }
#endif

    std::lock_guard<std::mutex> lock(storeMutex);
    if (!ensureLeagueAccess(callback, accountEmail, leagueId)) {
        return;
    }
    auto league = leaguesById[leagueId].league.toJson();
    league["members"] = arrayForLeague(membersByLeague, leagueId);
    callback(jsonResponse(league, drogon::k200OK));
}

void handleUpdateLeague(const drogon::HttpRequestPtr &req,
                        std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                        const std::string &accountEmail,
                        const std::string &leagueId) {
    const auto body = req->getJsonObject();
    if (!body || !body->isObject()) {
        sendError(callback, drogon::k400BadRequest, "League payload is required");
        return;
    }

    auto updated = cff::League::fromJson(normalizeLeaguePayload(*body));
    updated.id = leagueId;
    if (!draftDateAtTopOfHour(updated.draftDate)) {
        sendError(callback, drogon::k400BadRequest, "Draft time must be scheduled at the top of an hour");
        return;
    }

#ifdef CFF_HAS_POSTGRES
    if (dbConfigured()) {
        auto saved = dbUpdateLeague(accountEmail, leagueId, updated);
        if (!saved) {
            sendError(callback, drogon::k404NotFound, "League not found");
            return;
        }
        callback(jsonResponse(*saved, drogon::k200OK));
        return;
    }
#endif

    std::lock_guard<std::mutex> lock(storeMutex);
    if (!ensureCommissionerAccess(callback, accountEmail, leagueId)) {
        return;
    }
    leaguesById[leagueId].league = updated;
    auto &members = arrayForLeague(membersByLeague, leagueId);
    for (const auto &email : updated.invitedEmails) {
        bool exists = false;
        for (const auto &member : members) {
            exists = exists || jsonString(member, "email") == email;
        }
        if (!exists) {
            Json::Value member;
            member["email"] = email;
            member["role"] = "member";
            member["status"] = "Invited";
            members.append(member);
        }
    }
    auto payload = updated.toJson();
    payload["members"] = members;
    callback(jsonResponse(payload, drogon::k200OK));
}

void handleDeleteLeague(const drogon::HttpRequestPtr&,
                        std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                        const std::string &accountEmail,
                        const std::string &leagueId) {
#ifdef CFF_HAS_POSTGRES
    if (dbConfigured()) {
        auto deleted = dbDeleteLeague(accountEmail, leagueId);
        if (!deleted) {
            sendError(callback, drogon::k500InternalServerError, "Unable to delete league");
            return;
        }
        if (!*deleted) {
            sendError(callback, drogon::k404NotFound, "League not found");
            return;
        }
        Json::Value payload;
        payload["deleted"] = true;
        callback(jsonResponse(payload, drogon::k200OK));
        return;
    }
#endif

    std::lock_guard<std::mutex> lock(storeMutex);
    if (!ensureCommissionerAccess(callback, accountEmail, leagueId)) {
        return;
    }
    leaguesById.erase(leagueId);
    for (auto &[_, ownerLeagues] : leagueIdsByOwner) {
        ownerLeagues.erase(std::remove(ownerLeagues.begin(), ownerLeagues.end(), leagueId), ownerLeagues.end());
    }
    rostersByLeague.erase(leagueId);
    waiversByLeague.erase(leagueId);
    tradesByLeague.erase(leagueId);
    transactionsByLeague.erase(leagueId);
    membersByLeague.erase(leagueId);
    Json::Value payload;
    payload["deleted"] = true;
    callback(jsonResponse(payload, drogon::k200OK));
}

void handleListMembers(const drogon::HttpRequestPtr&,
                       std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                       const std::string &accountEmail,
                       const std::string &leagueId) {
#ifdef CFF_HAS_POSTGRES
    if (dbConfigured()) {
        auto members = dbListMembers(accountEmail, leagueId);
        if (!members) {
            sendError(callback, drogon::k404NotFound, "League not found");
            return;
        }
        callback(jsonResponse(*members, drogon::k200OK));
        return;
    }
#endif

    std::lock_guard<std::mutex> lock(storeMutex);
    if (!ensureCommissionerAccess(callback, accountEmail, leagueId)) {
        return;
    }
    auto &members = arrayForLeague(membersByLeague, leagueId);
    if (members.empty()) {
        Json::Value owner;
        owner["email"] = accountEmail;
        owner["role"] = "commissioner";
        owner["status"] = "Active";
        owner["teamName"] = "";
        members.append(owner);
    }
    callback(jsonResponse(members, drogon::k200OK));
}

void handleInviteMember(const drogon::HttpRequestPtr &req,
                        std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                        const std::string &accountEmail,
                        const std::string &leagueId) {
    const auto body = req->getJsonObject();
    const auto email = body ? canonicalEmail(jsonString(*body, "email")) : "";
    const auto role = body ? jsonString(*body, "role", "member") : "member";
    if (email.empty()) {
        sendError(callback, drogon::k400BadRequest, "email is required");
        return;
    }

#ifdef CFF_HAS_POSTGRES
    if (dbConfigured()) {
        auto members = dbInviteMember(accountEmail, leagueId, email, role);
        if (!members) {
            sendError(callback, drogon::k403Forbidden, "Commissioner access required");
            return;
        }
        callback(jsonResponse(*members, drogon::k201Created));
        return;
    }
#endif

    std::lock_guard<std::mutex> lock(storeMutex);
    if (!ensureCommissionerAccess(callback, accountEmail, leagueId)) {
        return;
    }
    auto &members = arrayForLeague(membersByLeague, leagueId);
    Json::Value member;
    member["email"] = email;
    member["role"] = role == "commissioner" ? "commissioner" : "member";
    member["status"] = "Invited";
    member["teamName"] = "";
    members.append(member);
    callback(jsonResponse(members, drogon::k201Created));
}

void handleUpdateMember(const drogon::HttpRequestPtr &req,
                        std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                        const std::string &accountEmail,
                        const std::string &leagueId,
                        const std::string &memberEmail) {
    const auto body = req->getJsonObject();
    const auto normalizedMemberEmail = canonicalEmail(memberEmail);
    const auto role = body ? jsonString(*body, "role", "member") : "member";
    const auto status = body ? jsonString(*body, "status", "Invited") : "Invited";
    const auto teamName = body ? jsonString(*body, "teamName", "") : "";
    const auto updateTeamName = body && body->isMember("teamName");

#ifdef CFF_HAS_POSTGRES
    if (dbConfigured()) {
        auto members = dbUpdateMember(accountEmail, leagueId, normalizedMemberEmail, role, status, teamName, updateTeamName);
        if (!members) {
            sendError(callback, drogon::k403Forbidden, "Commissioner access required");
            return;
        }
        callback(jsonResponse(*members, drogon::k200OK));
        return;
    }
#endif

    std::lock_guard<std::mutex> lock(storeMutex);
    if (!ensureCommissionerAccess(callback, accountEmail, leagueId)) {
        return;
    }
    const auto safeRole = role == "commissioner" ? "commissioner" : "member";
    const auto safeStatus = lowerString(status);
    const auto ownerTarget = ownsLeagueLocked(normalizedMemberEmail, leagueId);
    if (ownerTarget && (!updateTeamName || safeRole != "commissioner" || safeStatus != "active")) {
        sendError(callback, drogon::k403Forbidden, "The league owner cannot be demoted or removed");
        return;
    }
    if (safeRole == "commissioner" && !ownsLeagueLocked(accountEmail, leagueId)) {
        sendError(callback, drogon::k403Forbidden, "Only the league owner may grant commissioner access");
        return;
    }
    auto &members = arrayForLeague(membersByLeague, leagueId);
    for (Json::ArrayIndex i = 0; i < members.size(); ++i) {
        if (canonicalEmail(jsonString(members[i], "email")) == normalizedMemberEmail) {
            if (!ownerTarget) {
                members[i]["role"] = safeRole;
                members[i]["status"] = status;
            }
            if (updateTeamName) {
                members[i]["teamName"] = teamName;
            }
            callback(jsonResponse(members, drogon::k200OK));
            return;
        }
    }
    sendError(callback, drogon::k404NotFound, "Member not found");
}

void handleJoinLeague(const drogon::HttpRequestPtr&,
                      std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                      const std::string &accountEmail,
                      const std::string &leagueId) {
#ifdef CFF_HAS_POSTGRES
    if (dbConfigured()) {
        auto result = dbJoinLeague(accountEmail, leagueId);
        if (!result) {
            sendError(callback, drogon::k403Forbidden, "Invite not found for this account");
            return;
        }
        const auto pending = result->isMember("joinStatus") && (*result)["joinStatus"].asString() == "pending_approval";
        callback(jsonResponse(*result, pending ? static_cast<drogon::HttpStatusCode>(202) : drogon::k200OK));
        return;
    }
#endif
    std::lock_guard<std::mutex> lock(storeMutex);
    const auto it = leaguesById.find(leagueId);
    if (it == leaguesById.end()) {
        sendError(callback, drogon::k404NotFound, "League not found");
        return;
    }
    auto &members = arrayForLeague(membersByLeague, leagueId);
    bool invited = false;
    for (Json::ArrayIndex i = 0; i < members.size(); ++i) {
        if (canonicalEmail(jsonString(members[i], "email")) == canonicalEmail(accountEmail)) {
            const auto status = lowerString(jsonString(members[i], "status"));
            invited = status == "invited" || status == "pending" || status == "active";
            if (status == "active") {
                auto league = it->second.league.toJson();
                league["members"] = members;
                league["joinStatus"] = "active";
                callback(jsonResponse(league, drogon::k200OK));
                return;
            }
            if (status == "invited") {
                members[i]["status"] = "Pending";
            }
            break;
        }
    }
    if (!invited) {
        sendError(callback, drogon::k403Forbidden, "Invite not found for this account");
        return;
    }
    Json::Value pending;
    pending["id"] = leagueId;
    pending["joinStatus"] = "pending_approval";
    pending["message"] = "Join request submitted. A commissioner must approve access.";
    callback(jsonResponse(pending, static_cast<drogon::HttpStatusCode>(202)));
}

void handleGetRoster(const drogon::HttpRequestPtr&,
                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                     const std::string &accountEmail,
                     const std::string &leagueId) {
#ifdef CFF_HAS_POSTGRES
    if (dbConfigured()) {
        auto roster = dbGetRoster(accountEmail, leagueId);
        if (!roster) {
            sendError(callback, drogon::k404NotFound, "League not found");
            return;
        }
        callback(jsonResponse(*roster, drogon::k200OK));
        return;
    }
#endif

    std::lock_guard<std::mutex> lock(storeMutex);
    if (!ensureLeagueAccess(callback, accountEmail, leagueId)) {
        return;
    }
    callback(jsonResponse(arrayForLeague(rostersByLeague, leagueId), drogon::k200OK));
}

void handleGetManagerRoster(const drogon::HttpRequestPtr&,
                            std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                            const std::string &accountEmail,
                            const std::string &leagueId,
                            const std::string &managerEmail) {
#ifdef CFF_HAS_POSTGRES
    if (dbConfigured()) {
        auto roster = dbGetManagerRoster(accountEmail, leagueId, managerEmail);
        if (!roster) {
            sendError(callback, drogon::k404NotFound, "Manager roster not found");
            return;
        }
        callback(jsonResponse(*roster, drogon::k200OK));
        return;
    }
#endif

    std::lock_guard<std::mutex> lock(storeMutex);
    if (!ensureLeagueAccess(callback, accountEmail, leagueId)) {
        return;
    }
    callback(jsonResponse(arrayForLeague(rostersByLeague, leagueId), drogon::k200OK));
}

void handleAddRosterPlayer(const drogon::HttpRequestPtr &req,
                           std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                           const std::string &accountEmail,
                           const std::string &leagueId) {
    const auto body = req->getJsonObject();
    if (!body || !body->isObject()) {
        sendError(callback, drogon::k400BadRequest, "Player payload is required");
        return;
    }

    auto playerPayload = normalizePlayerJson(body->isMember("player") ? (*body)["player"] : *body);
#ifdef CFF_HAS_POSTGRES
    if (dbConfigured()) {
        auto conn = connectToDb();
        if (conn && dbLineupLocked(conn.get(), leagueId)) {
            sendError(callback, drogon::k409Conflict, "Lineups are locked after finalized matchups");
            return;
        }
        const auto waiverRules = dbWaiverRules(leagueId).value_or(Json::Value{Json::objectValue});
        if (cff::league_waiver::modeActive(waiverRules)) {
            sendError(callback, drogon::k409Conflict, "Free agency is locked. Submit a waiver claim.");
            return;
        }
        auto roster = dbAddRosterPlayer(accountEmail, leagueId, playerPayload);
        if (!roster) {
            sendError(callback, drogon::k404NotFound, "League not found");
            return;
        }
        callback(jsonResponse(*roster, drogon::k200OK));
        return;
    }
#endif

    std::lock_guard<std::mutex> lock(storeMutex);
    if (!ensureLeagueAccess(callback, accountEmail, leagueId)) {
        return;
    }
    if (lineupLockedLocked(leagueId)) {
        sendError(callback, drogon::k409Conflict, "Lineups are locked after finalized matchups");
        return;
    }
    if (cff::league_waiver::modeActive(waiverRulesForLeagueLocked(leagueId))) {
        sendError(callback, drogon::k409Conflict, "Free agency is locked. Submit a waiver claim.");
        return;
    }
    auto &roster = arrayForLeague(rostersByLeague, leagueId);
    auto player = playerPayload;
    if (indexOfPlayer(roster, jsonString(player, "id")) < 0) {
        roster.append(player);
        addTransactionLocked(leagueId, "Free Agent", "Added " + jsonString(player, "name"), accountEmail);
    }
    callback(jsonResponse(roster, drogon::k200OK));
}

void handleDropRosterPlayer(const drogon::HttpRequestPtr &req,
                            std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                            const std::string &accountEmail,
                            const std::string &leagueId) {
    const auto body = req->getJsonObject();
    const auto playerId = body ? jsonString(*body, "playerId") : "";
    if (playerId.empty()) {
        sendError(callback, drogon::k400BadRequest, "playerId is required");
        return;
    }
#ifdef CFF_HAS_POSTGRES
    if (dbConfigured()) {
        auto conn = connectToDb();
        if (conn && dbLineupLocked(conn.get(), leagueId)) {
            sendError(callback, drogon::k409Conflict, "Lineups are locked after finalized matchups");
            return;
        }
        if (conn && dbPlayerLockedInTrade(conn.get(), leagueId, accountEmail, playerId)) {
            sendError(callback, drogon::k409Conflict, "Player is locked in a pending trade");
            return;
        }
        auto roster = dbDropRosterPlayer(accountEmail, leagueId, playerId);
        if (!roster) {
            sendError(callback, drogon::k404NotFound, "League not found");
            return;
        }
        callback(jsonResponse(*roster, drogon::k200OK));
        return;
    }
#endif
    std::lock_guard<std::mutex> lock(storeMutex);
    if (!ensureLeagueAccess(callback, accountEmail, leagueId)) {
        return;
    }
    if (lineupLockedLocked(leagueId)) {
        sendError(callback, drogon::k409Conflict, "Lineups are locked after finalized matchups");
        return;
    }
    if (cff::league_trade::playerLockedInOpenOffer(
            arrayForLeague(tradesByLeague, leagueId), accountEmail, playerId)) {
        sendError(callback, drogon::k409Conflict, "Player is locked in a pending trade");
        return;
    }
    auto &roster = arrayForLeague(rostersByLeague, leagueId);
    auto removed = removePlayer(roster, playerId);
    if (removed.isObject()) {
        addTransactionLocked(leagueId, "Drop", "Dropped " + jsonString(removed, "name"), accountEmail);
    }
    callback(jsonResponse(roster, drogon::k200OK));
}

void handleUpdateRosterSlot(const drogon::HttpRequestPtr &req,
                            std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                            const std::string &accountEmail,
                            const std::string &leagueId,
                            const std::string &playerId) {
    const auto body = req->getJsonObject();
    const auto requestedSlot = body ? lowerString(jsonString(*body, "slot")) : "";
    if (playerId.empty() || requestedSlot.empty()) {
        sendError(callback, drogon::k400BadRequest, "playerId and slot are required");
        return;
    }
#ifdef CFF_HAS_POSTGRES
    if (dbConfigured()) {
        auto conn = connectToDb();
        if (conn && dbLineupLocked(conn.get(), leagueId)) {
            sendError(callback, drogon::k409Conflict, "Lineups are locked after finalized matchups");
            return;
        }
        auto roster = dbUpdateRosterSlot(accountEmail, leagueId, playerId, requestedSlot);
        if (!roster) {
            sendError(callback, drogon::k404NotFound, "Player roster entry not found");
            return;
        }
        if (roster->isObject() && roster->isMember("error")) {
            sendError(callback, drogon::k400BadRequest, jsonString(*roster, "error"));
            return;
        }
        callback(jsonResponse(*roster, drogon::k200OK));
        return;
    }
#endif

    std::lock_guard<std::mutex> lock(storeMutex);
    if (!ensureLeagueAccess(callback, accountEmail, leagueId)) {
        return;
    }
    if (lineupLockedLocked(leagueId)) {
        sendError(callback, drogon::k409Conflict, "Lineups are locked after finalized matchups");
        return;
    }
    if (cff::league_waiver::modeActive(waiverRulesForLeagueLocked(leagueId))) {
        sendError(callback, drogon::k409Conflict, "Free agency is locked. Submit a waiver claim.");
        return;
    }
    auto &roster = arrayForLeague(rostersByLeague, leagueId);
    const auto playerIndex = indexOfPlayer(roster, playerId);
    if (playerIndex < 0) {
        sendError(callback, drogon::k404NotFound, "Player roster entry not found");
        return;
    }
    const auto leagueIt = leaguesById.find(leagueId);
    const auto rules = leagueIt != leaguesById.end()
                           ? leagueIt->second.league.rosterRules.toJson()
                           : cff::RosterRules{}.toJson();
    if (!cff::league_roster::validateRosterSlotMove(roster[playerIndex], roster, rules, playerId, requestedSlot)) {
        sendError(callback, drogon::k400BadRequest, "Invalid roster slot");
        return;
    }
    roster[playerIndex]["rosterSlot"] = requestedSlot;
    callback(jsonResponse(roster, drogon::k200OK));
}

void handleFreeAgents(const drogon::HttpRequestPtr&,
                      std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                      const std::string &accountEmail,
                      const std::string &leagueId) {
#ifdef CFF_HAS_POSTGRES
    if (dbConfigured()) {
        auto agents = dbFreeAgents(accountEmail, leagueId);
        if (!agents) {
            sendError(callback, drogon::k404NotFound, "League not found");
            return;
        }
        callback(jsonResponse(*agents, drogon::k200OK));
        return;
    }
#endif

    std::lock_guard<std::mutex> lock(storeMutex);
    if (!ensureLeagueAccess(callback, accountEmail, leagueId)) {
        return;
    }
    const auto &roster = arrayForLeague(rostersByLeague, leagueId);
    Json::Value available(Json::arrayValue);
    for (const auto &player : sampleFreeAgentPool()) {
        if (indexOfPlayer(roster, jsonString(player, "id")) < 0) {
            available.append(player);
        }
    }
    callback(jsonResponse(available, drogon::k200OK));
}

void handleGetDraftState(const drogon::HttpRequestPtr&,
                         std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                         const std::string &accountEmail,
                         const std::string &leagueId) {
#ifdef CFF_HAS_POSTGRES
    if (dbConfigured()) {
        auto state = dbGetDraftState(accountEmail, leagueId);
        if (!state) {
            sendError(callback, drogon::k404NotFound, "League not found");
            return;
        }
        callback(jsonResponse(*state, drogon::k200OK));
        return;
    }
#endif

    std::lock_guard<std::mutex> lock(storeMutex);
    if (!ensureLeagueAccess(callback, accountEmail, leagueId)) return;
    callback(jsonResponse(localDraftPayloadLocked(accountEmail, leagueId), drogon::k200OK));
}

void handleSaveDraftQueue(const drogon::HttpRequestPtr &req,
                          std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                          const std::string &accountEmail,
                          const std::string &leagueId) {
    const auto body = req->getJsonObject();
    const auto queue = body && body->isMember("queue") && (*body)["queue"].isArray() ? (*body)["queue"] : Json::Value{Json::arrayValue};
#ifdef CFF_HAS_POSTGRES
    if (dbConfigured()) {
        auto state = dbSaveDraftQueue(accountEmail, leagueId, queue);
        if (!state) {
            sendError(callback, drogon::k404NotFound, "League not found");
            return;
        }
        callback(jsonResponse(*state, drogon::k200OK));
        return;
    }
#endif

    std::lock_guard<std::mutex> lock(storeMutex);
    if (!ensureLeagueAccess(callback, accountEmail, leagueId)) return;
    draftQueuesByLeagueManager[leagueId + ":" + accountEmail] = queue;
    callback(jsonResponse(localDraftPayloadLocked(accountEmail, leagueId), drogon::k200OK));
}

void handleSaveDraftOrder(const drogon::HttpRequestPtr &req,
                          std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                          const std::string &accountEmail,
                          const std::string &leagueId) {
    const auto body = req->getJsonObject();
    const auto order = body && body->isMember("draftOrder") && (*body)["draftOrder"].isArray()
                           ? (*body)["draftOrder"]
                           : Json::Value{Json::arrayValue};
#ifdef CFF_HAS_POSTGRES
    if (dbConfigured()) {
        auto state = dbSaveDraftOrder(accountEmail, leagueId, order);
        if (!state) {
            sendError(callback, drogon::k409Conflict, "Unable to save draft order");
            return;
        }
        callback(jsonResponse(*state, drogon::k200OK));
        return;
    }
#endif

    std::lock_guard<std::mutex> lock(storeMutex);
    if (!ensureCommissionerAccess(callback, accountEmail, leagueId)) return;
    auto &state = localDraftStateLocked(leagueId);
    if (state["status"].asString() != "not_started"
        || !arrayForLeague(draftPicksByLeague, leagueId).empty()
        || !localDraftOrderMatchesActiveMembers(leagueId, order)) {
        sendError(callback, drogon::k409Conflict, "Unable to save draft order");
        return;
    }
    state["draftOrder"] = order;
    state["currentPick"] = 1;
    state["pickDeadline"] = "";
    state["startedAt"] = "";
    callback(jsonResponse(localDraftPayloadLocked(accountEmail, leagueId), drogon::k200OK));
}

void handleStartDraft(const drogon::HttpRequestPtr&,
                      std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                      const std::string &accountEmail,
                      const std::string &leagueId) {
#ifdef CFF_HAS_POSTGRES
    if (dbConfigured()) {
        auto state = dbStartDraft(accountEmail, leagueId);
        if (!state) {
            sendError(callback, drogon::k403Forbidden, "Commissioner access and an open lobby with at least two active managers are required");
            return;
        }
        callback(jsonResponse(*state, drogon::k200OK));
        return;
    }
#endif

    std::lock_guard<std::mutex> lock(storeMutex);
    if (!ensureCommissionerAccess(callback, accountEmail, leagueId)) return;
    auto &state = localDraftStateLocked(leagueId);
    if (!state["lobbyOpen"].asBool()) {
        sendError(callback, drogon::k403Forbidden, "Draft lobby is not open");
        return;
    }
    if (state["status"].asString() == "open") {
        callback(jsonResponse(localDraftPayloadLocked(accountEmail, leagueId), drogon::k200OK));
        return;
    }
    auto order = state["draftOrder"];
    if (!localDraftOrderMatchesActiveMembers(leagueId, order)) {
        order = activeDraftOrderLocked(leagueId);
    }
    if (order.size() < 2 || !localDraftOrderMatchesActiveMembers(leagueId, order)
        || !arrayForLeague(draftPicksByLeague, leagueId).empty()) {
        sendError(callback, drogon::k409Conflict, "At least two active managers and a valid draft order are required");
        return;
    }
    state["status"] = "open";
    state["currentPick"] = 1;
    state["draftOrder"] = order;
    state["startedAt"] = draftIsoNow();
    state["pickDeadline"] = draftDeadlineFromNow(state.get("pickClockSeconds", 90).asInt());
    callback(jsonResponse(localDraftPayloadLocked(accountEmail, leagueId), drogon::k200OK));
}

void handleMakeDraftPick(const drogon::HttpRequestPtr &req,
                         std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                         const std::string &accountEmail,
                         const std::string &leagueId) {
    const auto body = req->getJsonObject();
    if (!body || !body->isObject() || !body->isMember("player")) {
        sendError(callback, drogon::k400BadRequest, "player is required");
        return;
    }
#ifdef CFF_HAS_POSTGRES
    if (dbConfigured()) {
        auto state = dbMakeDraftPick(accountEmail, leagueId, (*body)["player"]);
        if (!state) {
            sendError(callback, drogon::k409Conflict, "Unable to make draft pick");
            return;
        }
        callback(jsonResponse(*state, drogon::k201Created));
        return;
    }
#endif

    std::lock_guard<std::mutex> lock(storeMutex);
    if (!ensureLeagueAccess(callback, accountEmail, leagueId)) return;
    auto &state = localDraftStateLocked(leagueId);
    if (!state["lobbyOpen"].asBool() || state["status"].asString() != "open") {
        sendError(callback, drogon::k409Conflict, "Draft has not started");
        return;
    }
    const auto expectedManager = cff::league_schedule::currentDraftManager(
        state["draftOrder"], state.get("currentPick", 1).asInt(), state.get("draftType", "snake").asString());
    if (!expectedManager.empty() && canonicalEmail(expectedManager) != canonicalEmail(accountEmail)) {
        sendError(callback, drogon::k409Conflict, "It is not this manager's turn");
        return;
    }
    auto player = normalizePlayerJson((*body)["player"]);
    const auto playerId = jsonString(player, "id");
    if (playerId.empty()) {
        sendError(callback, drogon::k400BadRequest, "player id is required");
        return;
    }
    auto &picks = arrayForLeague(draftPicksByLeague, leagueId);
    for (const auto &existing : picks) {
        if (jsonString(existing["player"], "id") == playerId) {
            sendError(callback, drogon::k409Conflict, "Player has already been drafted");
            return;
        }
    }
    Json::Value pick;
    pick["id"] = timestampId("pick");
    pick["managerEmail"] = accountEmail;
    pick["pickNumber"] = static_cast<int>(picks.size()) + 1;
    pick["player"] = player;
    pick["createdAt"] = draftIsoNow();
    picks.append(pick);
    auto &roster = arrayForLeague(rostersByLeague, leagueId);
    if (indexOfPlayer(roster, playerId) < 0) roster.append(player);
    auto &queue = arrayForLeague(draftQueuesByLeagueManager, leagueId + ":" + accountEmail);
    removePlayer(queue, playerId);
    addTransactionLocked(leagueId, "Draft Pick", "Drafted " + jsonString(player, "name"), accountEmail);
    state["currentPick"] = static_cast<int>(picks.size()) + 1;
    state["pickDeadline"] = draftDeadlineFromNow(state.get("pickClockSeconds", 90).asInt());
    callback(jsonResponse(localDraftPayloadLocked(accountEmail, leagueId), drogon::k201Created));
}

void handleResetDraft(const drogon::HttpRequestPtr&,
                      std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                      const std::string &accountEmail,
                      const std::string &leagueId) {
#ifdef CFF_HAS_POSTGRES
    if (dbConfigured()) {
        auto state = dbResetDraft(accountEmail, leagueId);
        if (!state) {
            sendError(callback, drogon::k403Forbidden, "Commissioner access required");
            return;
        }
        callback(jsonResponse(*state, drogon::k200OK));
        return;
    }
#endif

    std::lock_guard<std::mutex> lock(storeMutex);
    if (!ensureCommissionerAccess(callback, accountEmail, leagueId)) return;
    draftPicksByLeague[leagueId] = Json::Value{Json::arrayValue};
    rostersByLeague[leagueId] = Json::Value{Json::arrayValue};
    auto &state = localDraftStateLocked(leagueId);
    state["status"] = "not_started";
    state["currentPick"] = 1;
    state["pickDeadline"] = "";
    state["startedAt"] = "";
    callback(jsonResponse(localDraftPayloadLocked(accountEmail, leagueId), drogon::k200OK));
}

void handleUndoDraftPick(const drogon::HttpRequestPtr&,
                         std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                         const std::string &accountEmail,
                         const std::string &leagueId) {
#ifdef CFF_HAS_POSTGRES
    if (dbConfigured()) {
        auto state = dbUndoDraftPick(accountEmail, leagueId);
        if (!state) {
            sendError(callback, drogon::k403Forbidden, "Commissioner access required");
            return;
        }
        callback(jsonResponse(*state, drogon::k200OK));
        return;
    }
#endif

    std::lock_guard<std::mutex> lock(storeMutex);
    if (!ensureCommissionerAccess(callback, accountEmail, leagueId)) return;
    auto &picks = arrayForLeague(draftPicksByLeague, leagueId);
    auto &state = localDraftStateLocked(leagueId);
    if (!picks.empty()) {
        Json::Value removed;
        picks.removeIndex(static_cast<Json::ArrayIndex>(picks.size() - 1), &removed);
        const auto managerEmail = jsonString(removed, "managerEmail", accountEmail);
        const auto player = removed["player"];
        const auto playerId = jsonString(player, "id");
        auto &roster = arrayForLeague(rostersByLeague, leagueId);
        removePlayer(roster, playerId);
        auto &queue = arrayForLeague(draftQueuesByLeagueManager, leagueId + ":" + managerEmail);
        if (indexOfPlayer(queue, playerId) < 0) queue.append(player);
        state["status"] = "open";
        state["currentPick"] = static_cast<int>(picks.size()) + 1;
        state["pickDeadline"] = draftDeadlineFromNow(state.get("pickClockSeconds", 90).asInt());
    }
    callback(jsonResponse(localDraftPayloadLocked(accountEmail, leagueId), drogon::k200OK));
}

void handleListWaivers(const drogon::HttpRequestPtr&,
                       std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                       const std::string &accountEmail,
                       const std::string &leagueId) {
#ifdef CFF_HAS_POSTGRES
    if (dbConfigured()) {
        auto claims = dbListWaivers(accountEmail, leagueId);
        if (!claims) {
            sendError(callback, drogon::k404NotFound, "League not found");
            return;
        }
        callback(jsonResponse(*claims, drogon::k200OK));
        return;
    }
#endif

    std::lock_guard<std::mutex> lock(storeMutex);
    if (!ensureLeagueAccess(callback, accountEmail, leagueId)) {
        return;
    }
    callback(jsonResponse(arrayForLeague(waiversByLeague, leagueId), drogon::k200OK));
}

void handleCreateWaiver(const drogon::HttpRequestPtr &req,
                        std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                        const std::string &accountEmail,
                        const std::string &leagueId) {
    const auto body = req->getJsonObject();
    if (!body || !body->isObject() || !body->isMember("addPlayer")) {
        sendError(callback, drogon::k400BadRequest, "addPlayer is required");
        return;
    }
#ifdef CFF_HAS_POSTGRES
    if (dbConfigured()) {
        auto conn = connectToDb();
        if (conn && dbCanAccessLeague(accountEmail, leagueId) && dbLineupLocked(conn.get(), leagueId)) {
            sendError(callback, drogon::k409Conflict, "Waivers are locked after finalized matchups");
            return;
        }
        auto claim = dbCreateWaiver(accountEmail, leagueId, *body);
        if (!claim) {
            sendError(callback, drogon::k404NotFound, "League not found");
            return;
        }
        callback(jsonResponse(*claim, drogon::k201Created));
        return;
    }
#endif
    std::lock_guard<std::mutex> lock(storeMutex);
    if (!ensureLeagueAccess(callback, accountEmail, leagueId)) {
        return;
    }
    if (lineupLockedLocked(leagueId)) {
        sendError(callback, drogon::k409Conflict, "Waivers are locked after finalized matchups");
        return;
    }
    auto &roster = arrayForLeague(rostersByLeague, leagueId);
    const auto addPlayer = normalizePlayerJson((*body)["addPlayer"]);
    const auto addPlayerId = jsonString(addPlayer, "id");
    const auto dropPlayerId = jsonString(*body, "dropPlayerId");
    if (indexOfPlayer(roster, addPlayerId) >= 0) {
        sendError(callback, drogon::k409Conflict, "Player is already rostered");
        return;
    }
    if (!dropPlayerId.empty() && indexOfPlayer(roster, dropPlayerId) < 0) {
        sendError(callback, drogon::k400BadRequest, "Drop player must be on your roster");
        return;
    }
    Json::Value claim;
    claim["id"] = timestampId("waiver");
    claim["addPlayer"] = addPlayer;
    claim["dropPlayerId"] = dropPlayerId;
    claim["status"] = "Pending";
    claim["createdAt"] = timestampId("at");
    claim["managerEmail"] = accountEmail;
    auto &claims = arrayForLeague(waiversByLeague, leagueId);
    const auto claimOrder = cff::league_waiver::nextClaimOrder(claims, accountEmail);
    claim["priority"] = 1;
    claim["claimOrder"] = claimOrder;
    claims.insert(0, claim);
    addTransactionLocked(leagueId, "Waiver Claim", cff::league_waiver::claimTransactionSummary(claim["addPlayer"]), accountEmail);
    callback(jsonResponse(claim, drogon::k201Created));
}

void handleProcessWaiver(const drogon::HttpRequestPtr&,
                         std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                         const std::string &accountEmail,
                         const std::string &leagueId,
                         const std::string &claimId) {
#ifdef CFF_HAS_POSTGRES
    if (dbConfigured()) {
        auto conn = connectToDb();
        if (conn && dbCanAccessLeague(accountEmail, leagueId) && dbLineupLocked(conn.get(), leagueId)) {
            sendError(callback, drogon::k409Conflict, "Waivers are locked after finalized matchups");
            return;
        }
        const auto waiverRules = dbWaiverRules(leagueId).value_or(Json::Value{Json::objectValue});
        if (!cff::league_waiver::deadlinePassed(waiverRules)) {
            sendError(callback, drogon::k409Conflict, "Waiver deadline has not passed yet");
            return;
        }
        auto claim = dbProcessWaiver(accountEmail, leagueId, claimId);
        if (!claim) {
            sendError(callback, drogon::k404NotFound, "Waiver claim not found");
            return;
        }
        callback(jsonResponse(*claim, drogon::k200OK));
        return;
    }
#endif

    std::lock_guard<std::mutex> lock(storeMutex);
    if (!ensureLeagueAccess(callback, accountEmail, leagueId)) {
        return;
    }
    if (lineupLockedLocked(leagueId)) {
        sendError(callback, drogon::k409Conflict, "Waivers are locked after finalized matchups");
        return;
    }
    if (!cff::league_waiver::deadlinePassed(waiverRulesForLeagueLocked(leagueId))) {
        sendError(callback, drogon::k409Conflict, "Waiver deadline has not passed yet");
        return;
    }
    auto &claims = arrayForLeague(waiversByLeague, leagueId);
    auto &roster = arrayForLeague(rostersByLeague, leagueId);
    for (Json::ArrayIndex i = 0; i < claims.size(); ++i) {
        if (jsonString(claims[i], "id") == claimId) {
            const auto dropPlayerId = jsonString(claims[i], "dropPlayerId");
            auto player = normalizePlayerJson(claims[i]["addPlayer"]);
            if (indexOfPlayer(roster, jsonString(player, "id")) >= 0
                || (!dropPlayerId.empty() && indexOfPlayer(roster, dropPlayerId) < 0)) {
                claims[i]["status"] = "Cancelled";
                callback(jsonResponse(claims[i], drogon::k409Conflict));
                return;
            }
            if (!dropPlayerId.empty()) {
                removePlayer(roster, dropPlayerId);
            }
            if (indexOfPlayer(roster, jsonString(player, "id")) < 0) {
                roster.append(player);
            }
            claims[i]["status"] = "Processed";
            addTransactionLocked(leagueId, "Waiver Processed", cff::league_waiver::processedTransactionSummary(player), accountEmail);
            callback(jsonResponse(claims[i], drogon::k200OK));
            return;
        }
    }
    sendError(callback, drogon::k404NotFound, "Waiver claim not found");
}

void handleUpdateWaiverStatus(const drogon::HttpRequestPtr &req,
                              std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                              const std::string &accountEmail,
                              const std::string &leagueId,
                              const std::string &claimId) {
    const auto body = req->getJsonObject();
    const auto status = body ? jsonString(*body, "status") : "";
    if (status != "Cancelled") {
        sendError(callback, drogon::k400BadRequest, "status must be Cancelled");
        return;
    }
#ifdef CFF_HAS_POSTGRES
    if (dbConfigured()) {
        auto claims = dbUpdateWaiverStatus(accountEmail, leagueId, claimId, status);
        if (!claims) {
            sendError(callback, drogon::k404NotFound, "Waiver claim not found");
            return;
        }
        callback(jsonResponse(*claims, drogon::k200OK));
        return;
    }
#endif

    std::lock_guard<std::mutex> lock(storeMutex);
    if (!ensureLeagueAccess(callback, accountEmail, leagueId)) {
        return;
    }
    const bool commissioner = isCommissionerLocked(accountEmail, leagueId);
    auto &claims = arrayForLeague(waiversByLeague, leagueId);
    for (auto &claim : claims) {
        if (jsonString(claim, "id") != claimId || jsonString(claim, "status") != "Pending") continue;
        const auto managerEmail = jsonString(claim, "managerEmail", accountEmail);
        if (!commissioner && managerEmail != accountEmail) {
            sendError(callback, drogon::k403Forbidden, "Only claim owners can cancel waiver claims");
            return;
        }
        claim["status"] = "Cancelled";
        addTransactionLocked(leagueId, "Waiver Cancelled", cff::league_waiver::cancelledTransactionSummary(), accountEmail);
        callback(jsonResponse(claims, drogon::k200OK));
        return;
    }
    sendError(callback, drogon::k404NotFound, "Waiver claim not found");
}

void handleReorderWaivers(const drogon::HttpRequestPtr &req,
                          std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                          const std::string &accountEmail,
                          const std::string &leagueId) {
    const auto body = req->getJsonObject();
    const auto claimIds = body && body->isMember("claimIds") && (*body)["claimIds"].isArray()
                              ? (*body)["claimIds"]
                              : Json::Value{Json::arrayValue};
    if (!claimIds.size()) {
        sendError(callback, drogon::k400BadRequest, "claimIds are required");
        return;
    }
#ifdef CFF_HAS_POSTGRES
    if (dbConfigured()) {
        auto claims = dbReorderWaivers(accountEmail, leagueId, claimIds);
        if (!claims) {
            sendError(callback, drogon::k404NotFound, "Waiver claims not found");
            return;
        }
        callback(jsonResponse(*claims, drogon::k200OK));
        return;
    }
#endif

    std::lock_guard<std::mutex> lock(storeMutex);
    if (!ensureLeagueAccess(callback, accountEmail, leagueId)) {
        return;
    }
    auto &claims = arrayForLeague(waiversByLeague, leagueId);
    int order = 1;
    for (const auto &claimIdValue : claimIds) {
        if (!claimIdValue.isString()) continue;
        for (auto &claim : claims) {
            if (jsonString(claim, "id") == claimIdValue.asString()
                && jsonString(claim, "status") == "Pending"
                && jsonString(claim, "managerEmail", accountEmail) == accountEmail) {
                claim["claimOrder"] = order++;
            }
        }
    }
    callback(jsonResponse(claims, drogon::k200OK));
}

void handleProcessWaivers(const drogon::HttpRequestPtr&,
                          std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                          const std::string &accountEmail,
                          const std::string &leagueId) {
#ifdef CFF_HAS_POSTGRES
    if (dbConfigured()) {
        auto conn = connectToDb();
        if (conn && dbCanAccessLeague(accountEmail, leagueId) && dbLineupLocked(conn.get(), leagueId)) {
            sendError(callback, drogon::k409Conflict, "Waivers are locked after finalized matchups");
            return;
        }
        const auto waiverRules = dbWaiverRules(leagueId).value_or(Json::Value{Json::objectValue});
        if (!cff::league_waiver::deadlinePassed(waiverRules)) {
            sendError(callback, drogon::k409Conflict, "Waiver deadline has not passed yet");
            return;
        }
        auto result = dbProcessWaivers(accountEmail, leagueId);
        if (!result) {
            sendError(callback, drogon::k403Forbidden, "Commissioner access required");
            return;
        }
        callback(jsonResponse(*result, drogon::k200OK));
        return;
    }
#endif

    std::lock_guard<std::mutex> lock(storeMutex);
    if (!ensureCommissionerAccess(callback, accountEmail, leagueId)) {
        return;
    }
    if (lineupLockedLocked(leagueId)) {
        sendError(callback, drogon::k409Conflict, "Waivers are locked after finalized matchups");
        return;
    }
    if (!cff::league_waiver::deadlinePassed(waiverRulesForLeagueLocked(leagueId))) {
        sendError(callback, drogon::k409Conflict, "Waiver deadline has not passed yet");
        return;
    }
    auto &claims = arrayForLeague(waiversByLeague, leagueId);
    auto &roster = arrayForLeague(rostersByLeague, leagueId);
    Json::Value processed(Json::arrayValue);
    Json::Value cancelled(Json::arrayValue);
    const auto claimIndexes = cff::league_waiver::orderedClaimIndexes(claims);
    for (const auto i : claimIndexes) {
        if (jsonString(claims[i], "status") != "Pending") {
            continue;
        }
        const auto dropPlayerId = jsonString(claims[i], "dropPlayerId");
        auto player = normalizePlayerJson(claims[i]["addPlayer"]);
        if (indexOfPlayer(roster, jsonString(player, "id")) >= 0
            || (!dropPlayerId.empty() && indexOfPlayer(roster, dropPlayerId) < 0)) {
            claims[i]["status"] = "Cancelled";
            cancelled.append(jsonString(claims[i], "id"));
            continue;
        }
        if (!dropPlayerId.empty()) {
            removePlayer(roster, dropPlayerId);
        }
        if (indexOfPlayer(roster, jsonString(player, "id")) < 0) {
            roster.append(player);
        }
        claims[i]["status"] = "Processed";
        processed.append(jsonString(claims[i], "id"));
        addTransactionLocked(leagueId, "Waiver Processed", cff::league_waiver::processedTransactionSummary(player), accountEmail);
    }
    Json::Value payload;
    payload["processed"] = processed;
    payload["cancelled"] = cancelled;
    payload["claims"] = claims;
    callback(jsonResponse(payload, drogon::k200OK));
}

void handleListWaiverPriority(const drogon::HttpRequestPtr&,
                              std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                              const std::string &accountEmail,
                              const std::string &leagueId) {
#ifdef CFF_HAS_POSTGRES
    if (dbConfigured()) {
        auto priorities = dbWaiverPriorityBoard(accountEmail, leagueId);
        if (!priorities) {
            sendError(callback, drogon::k404NotFound, "League not found");
            return;
        }
        callback(jsonResponse(*priorities, drogon::k200OK));
        return;
    }
#endif

    std::lock_guard<std::mutex> lock(storeMutex);
    if (!ensureLeagueAccess(callback, accountEmail, leagueId)) {
        return;
    }
    auto priorities = cff::league_waiver::buildPriorityBoard(
        arrayForLeague(membersByLeague, leagueId));
    callback(jsonResponse(priorities, drogon::k200OK));
}

void handleResetWaiverPriority(const drogon::HttpRequestPtr&,
                               std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                               const std::string &accountEmail,
                               const std::string &leagueId) {
#ifdef CFF_HAS_POSTGRES
    if (dbConfigured()) {
        auto priorities = dbResetWaiverPriority(accountEmail, leagueId);
        if (!priorities) {
            sendError(callback, drogon::k403Forbidden, "Commissioner access required");
            return;
        }
        callback(jsonResponse(*priorities, drogon::k200OK));
        return;
    }
#endif

    std::lock_guard<std::mutex> lock(storeMutex);
    if (!ensureCommissionerAccess(callback, accountEmail, leagueId)) {
        return;
    }
    auto priorities = cff::league_waiver::buildPriorityBoard(
        arrayForLeague(membersByLeague, leagueId));
    addTransactionLocked(leagueId, "Waiver Priority", cff::league_waiver::resetPriorityTransactionSummary(), accountEmail);
    callback(jsonResponse(priorities, drogon::k200OK));
}

void handleListTrades(const drogon::HttpRequestPtr&,
                      std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                      const std::string &accountEmail,
                      const std::string &leagueId) {
#ifdef CFF_HAS_POSTGRES
    if (dbConfigured()) {
        auto trades = dbListTrades(accountEmail, leagueId);
        if (!trades) {
            sendError(callback, drogon::k404NotFound, "League not found");
            return;
        }
        callback(jsonResponse(*trades, drogon::k200OK));
        return;
    }
#endif

    std::lock_guard<std::mutex> lock(storeMutex);
    if (!ensureLeagueAccess(callback, accountEmail, leagueId)) {
        return;
    }
    callback(jsonResponse(arrayForLeague(tradesByLeague, leagueId), drogon::k200OK));
}

void handleCreateTrade(const drogon::HttpRequestPtr &req,
                       std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                       const std::string &accountEmail,
                       const std::string &leagueId) {
    const auto body = req->getJsonObject();
    if (!body || !body->isObject() || !body->isMember("offerPlayer")) {
        sendError(callback, drogon::k400BadRequest, "offerPlayer is required");
        return;
    }
    const auto requestedTarget = jsonString(*body, "targetManager", jsonString(*body, "targetManagerEmail"));
    if (!cff::league_trade::validTarget(accountEmail, requestedTarget)) {
        sendError(callback, drogon::k400BadRequest, "Trade target must be another active league manager");
        return;
    }
#ifdef CFF_HAS_POSTGRES
    if (dbConfigured()) {
        auto conn = connectToDb();
        if (conn && dbLineupLocked(conn.get(), leagueId)) {
            sendError(callback, drogon::k409Conflict, "Trades are locked after finalized matchups");
            return;
        }
        auto trade = dbCreateTrade(accountEmail, leagueId, *body);
        if (!trade) {
            sendError(callback, drogon::k403Forbidden, "Trade target must be an active league manager");
            return;
        }
        callback(jsonResponse(*trade, drogon::k201Created));
        return;
    }
#endif
    std::lock_guard<std::mutex> lock(storeMutex);
    if (!ensureLeagueAccess(callback, accountEmail, leagueId)) {
        return;
    }
    if (lineupLockedLocked(leagueId)) {
        sendError(callback, drogon::k409Conflict, "Trades are locked after finalized matchups");
        return;
    }
    const auto targetManager = jsonString(*body, "targetManager");
    if (!cff::league_trade::validTarget(accountEmail, targetManager)) {
        sendError(callback, drogon::k400BadRequest, "Trade target must be another active league manager");
        return;
    }
    if (!isActiveMemberLocked(accountEmail, leagueId) || !isActiveMemberLocked(targetManager, leagueId)) {
        sendError(callback, drogon::k403Forbidden, "Trade target must be an active league manager");
        return;
    }
    if (cff::league_trade::playerLockedInOpenOffer(
            arrayForLeague(tradesByLeague, leagueId),
            accountEmail,
            jsonString((*body)["offerPlayer"], "id"))) {
        sendError(callback, drogon::k409Conflict, "Player is locked in a pending trade");
        return;
    }
    auto &roster = arrayForLeague(rostersByLeague, leagueId);
    const auto offerId = jsonString((*body)["offerPlayer"], "id");
    const auto requestedId = body->isMember("requestPlayer") && (*body)["requestPlayer"].isObject()
                                 ? jsonString((*body)["requestPlayer"], "id")
                                 : "";
    if (offerId.empty() || indexOfPlayer(roster, offerId) < 0) {
        sendError(callback, drogon::k400BadRequest, "Offered player must be on your roster");
        return;
    }
    if (requestedId.empty() || requestedId == offerId) {
        sendError(callback, drogon::k400BadRequest, "Requested player must be selected from the target roster");
        return;
    }
    Json::Value trade;
    trade["id"] = timestampId("trade");
    trade["offerPlayer"] = normalizePlayerJson((*body)["offerPlayer"]);
    if (body->isMember("requestPlayer") && (*body)["requestPlayer"].isObject()) {
        trade["requestPlayer"] = normalizePlayerJson((*body)["requestPlayer"]);
    }
    trade["requestPlayerName"] = jsonString(*body, "requestPlayerName");
    trade["targetManager"] = targetManager;
    trade["note"] = jsonString(*body, "note");
    trade["offeredByEmail"] = accountEmail;
    trade["offeredToEmail"] = targetManager;
    const auto rules = tradeRulesForLeagueLocked(leagueId);
    trade["requiresApproval"] = cff::league_trade::approvalRequired(rules);
    trade["status"] = "Pending";
    trade["createdAt"] = timestampId("at");
    trade["expiresAt"] = timestampId("expires");
    auto &offers = arrayForLeague(tradesByLeague, leagueId);
    offers.insert(0, trade);
    addTransactionLocked(leagueId, "Trade Offer", cff::league_trade::offerTransactionSummary(trade["offerPlayer"]), accountEmail);
    callback(jsonResponse(trade, drogon::k201Created));
}

void handleUpdateTradeStatus(const drogon::HttpRequestPtr &req,
                             std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                             const std::string &accountEmail,
                             const std::string &leagueId,
                             const std::string &tradeId) {
    const auto body = req->getJsonObject();
    const auto status = body ? jsonString(*body, "status") : "";
    if (!cff::league_trade::requestStatusAllowed(status)) {
        sendError(callback, drogon::k400BadRequest, "status must be Accepted, Approved, Vetoed, Declined, or Cancelled");
        return;
    }
#ifdef CFF_HAS_POSTGRES
    if (dbConfigured()) {
        if (cff::league_trade::potentiallyExecutes(status)) {
            auto conn = connectToDb();
            if (conn && dbLineupLocked(conn.get(), leagueId)) {
                sendError(callback, drogon::k409Conflict, "Trades are locked after finalized matchups");
                return;
            }
        }
        auto trade = dbUpdateTradeStatus(accountEmail, leagueId, tradeId, status);
        if (!trade) {
            sendError(callback, drogon::k404NotFound, "Trade offer not found");
            return;
        }
        callback(jsonResponse(*trade, drogon::k200OK));
        return;
    }
#endif
    std::lock_guard<std::mutex> lock(storeMutex);
    if (!ensureLeagueAccess(callback, accountEmail, leagueId)) {
        return;
    }
    auto &offers = arrayForLeague(tradesByLeague, leagueId);
    for (Json::ArrayIndex i = 0; i < offers.size(); ++i) {
        if (jsonString(offers[i], "id") == tradeId) {
            const auto currentStatus = jsonString(offers[i], "status");
            if (!cff::league_trade::openStatus(currentStatus)) {
                sendError(callback, drogon::k409Conflict, "Trade is no longer open");
                return;
            }
            const bool requiresApproval = offers[i].isMember("requiresApproval") && offers[i]["requiresApproval"].asBool();
            const bool commissioner = isCommissionerLocked(accountEmail, leagueId);
            const auto decision = cff::league_trade::decideStatus(
                status, requiresApproval, true, commissioner, false);
            if (!decision.allowed) {
                if (decision.commissionerRequired) {
                    sendError(callback, drogon::k403Forbidden, "Commissioner access required");
                } else {
                    sendError(callback, drogon::k404NotFound, "Trade offer not found");
                }
                return;
            }
            const bool executeTrade = decision.execute;
            offers[i]["status"] = decision.displayStatus;
            if (executeTrade && lineupLockedLocked(leagueId)) {
                sendError(callback, drogon::k409Conflict, "Trades are locked after finalized matchups");
                return;
            }
            if (executeTrade && !offers[i].isMember("requestPlayer")) {
                sendError(callback, drogon::k409Conflict, "Trade players are no longer available");
                return;
            }
            if (executeTrade && offers[i].isMember("requestPlayer")) {
                auto &roster = arrayForLeague(rostersByLeague, leagueId);
                if (indexOfPlayer(roster, jsonString(offers[i]["offerPlayer"], "id")) < 0
                    || jsonString(offers[i]["requestPlayer"], "id").empty()) {
                    sendError(callback, drogon::k409Conflict, "Trade players are no longer available");
                    return;
                }
                removePlayer(roster, jsonString(offers[i]["offerPlayer"], "id"));
                if (indexOfPlayer(roster, jsonString(offers[i]["requestPlayer"], "id")) < 0) {
                    roster.append(normalizePlayerJson(offers[i]["requestPlayer"]));
                }
            }
            addTransactionLocked(leagueId, "Trade", cff::league_trade::statusTransactionSummary(decision.displayStatus, offers[i]["offerPlayer"]), accountEmail);
            callback(jsonResponse(offers[i], drogon::k200OK));
            return;
        }
    }
    sendError(callback, drogon::k404NotFound, "Trade offer not found");
}

void handleListTransactions(const drogon::HttpRequestPtr&,
                            std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                            const std::string &accountEmail,
                            const std::string &leagueId) {
#ifdef CFF_HAS_POSTGRES
    if (dbConfigured()) {
        auto txns = dbListTransactions(accountEmail, leagueId);
        if (!txns) {
            sendError(callback, drogon::k404NotFound, "League not found");
            return;
        }
        callback(jsonResponse(*txns, drogon::k200OK));
        return;
    }
#endif

    std::lock_guard<std::mutex> lock(storeMutex);
    if (!ensureLeagueAccess(callback, accountEmail, leagueId)) {
        return;
    }
    callback(jsonResponse(arrayForLeague(transactionsByLeague, leagueId), drogon::k200OK));
}

void handleListLeagueFeed(const drogon::HttpRequestPtr&,
                          std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                          const std::string &accountEmail,
                          const std::string &leagueId) {
#ifdef CFF_HAS_POSTGRES
    if (dbConfigured()) {
        auto feed = dbListLeagueFeed(accountEmail, leagueId);
        if (!feed) {
            sendError(callback, drogon::k404NotFound, "League not found");
            return;
        }
        callback(jsonResponse(*feed, drogon::k200OK));
        return;
    }
#endif

    std::lock_guard<std::mutex> lock(storeMutex);
    if (!ensureLeagueAccess(callback, accountEmail, leagueId)) {
        return;
    }
    callback(jsonResponse(buildLocalLeagueFeed(leagueId), drogon::k200OK));
}

void handleCreateLeagueFeedPost(const drogon::HttpRequestPtr &req,
                                std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                const std::string &accountEmail,
                                const std::string &leagueId) {
    const auto body = req->getJsonObject();
    const auto message = body ? jsonString(*body, "body") : "";
    if (message.empty() || message.size() > 2000) {
        sendError(callback, drogon::k400BadRequest, "Post body is required and must be 2000 characters or less");
        return;
    }

#ifdef CFF_HAS_POSTGRES
    if (dbConfigured()) {
        auto post = dbCreateLeagueFeedPost(accountEmail, leagueId, message);
        if (!post) {
            sendError(callback, drogon::k403Forbidden, "Commissioner access required");
            return;
        }
        callback(jsonResponse(*post, drogon::k201Created));
        return;
    }
#endif

    std::lock_guard<std::mutex> lock(storeMutex);
    if (!ensureCommissionerAccess(callback, accountEmail, leagueId)) {
        return;
    }
    Json::Value post;
    post["id"] = timestampId("feed-post");
    post["type"] = "Commissioner Post";
    post["summary"] = message;
    post["managerEmail"] = accountEmail;
    post["createdAt"] = timestampId("at");
    arrayForLeague(feedPostsByLeague, leagueId).insert(0, post);
    callback(jsonResponse(feedItem("Commissioner Post", message, jsonString(post, "createdAt"), accountEmail, "Post"), drogon::k201Created));
}

void handleListMatchups(const drogon::HttpRequestPtr&,
                        std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                        const std::string &accountEmail,
                        const std::string &leagueId) {
#ifdef CFF_HAS_POSTGRES
    if (dbConfigured()) {
        auto matchups = dbListMatchups(accountEmail, leagueId);
        if (!matchups) {
            sendError(callback, drogon::k404NotFound, "League not found");
            return;
        }
        callback(jsonResponse(*matchups, drogon::k200OK));
        return;
    }
#endif

    std::lock_guard<std::mutex> lock(storeMutex);
    if (!ensureLeagueAccess(callback, accountEmail, leagueId)) {
        return;
    }
    auto &matchups = arrayForLeague(matchupsByLeague, leagueId);
    if (!matchups.size()) {
        const auto &members = arrayForLeague(membersByLeague, leagueId);
        const auto scoreForManager = [&accountEmail, &leagueId](const std::string &managerEmail) {
            if (managerEmail != accountEmail) return 0.0;
            double total = 0.0;
            for (const auto &player : arrayForLeague(rostersByLeague, leagueId)) {
                if (lowerString(jsonString(player, "rosterSlot", "bench")) != "bench") {
                    total += projectionForPlayer(player);
                }
            }
            return total;
        };
        matchups = cff::league_schedule::buildMatchups(members, leagueId, 1, scoreForManager);
    }
    callback(jsonResponse(matchups, drogon::k200OK));
}

void handleGenerateMatchups(const drogon::HttpRequestPtr&,
                            std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                            const std::string &accountEmail,
                            const std::string &leagueId) {
#ifdef CFF_HAS_POSTGRES
    if (dbConfigured()) {
        if (!dbIsCommissioner(accountEmail, leagueId)) {
            sendError(callback, drogon::k403Forbidden, "Only commissioners can generate matchups");
            return;
        }
        auto matchups = dbGenerateMatchups(accountEmail, leagueId);
        if (!matchups) {
            sendError(callback, drogon::k404NotFound, "League not found");
            return;
        }
        callback(jsonResponse(*matchups, drogon::k200OK));
        return;
    }
#endif

    std::lock_guard<std::mutex> lock(storeMutex);
    if (!ensureCommissionerAccess(callback, accountEmail, leagueId, "Only commissioners can generate matchups")) {
        return;
    }
    const auto &members = arrayForLeague(membersByLeague, leagueId);
    const auto scoreForManager = [&accountEmail, &leagueId](const std::string &managerEmail) {
        if (managerEmail != accountEmail) return 0.0;
        double total = 0.0;
        for (const auto &player : arrayForLeague(rostersByLeague, leagueId)) {
            if (lowerString(jsonString(player, "rosterSlot", "bench")) != "bench") {
                total += projectionForPlayer(player);
            }
        }
        return total;
    };
    auto &matchups = arrayForLeague(matchupsByLeague, leagueId);
    for (const auto &matchup : matchups) {
        if (cff::getIntOrDefault(matchup, "week", 1) == 1 && lowerString(jsonString(matchup, "status", "scheduled")) == "final") {
            sendError(callback, drogon::k409Conflict, "Week is already final");
            return;
        }
    }
    matchups = cff::league_schedule::buildMatchups(members, leagueId, 1, scoreForManager);
    addTransactionLocked(leagueId, "Schedule", "Generated week 1 matchups", accountEmail);
    callback(jsonResponse(matchups, drogon::k200OK));
}

void handleGenerateSeasonSchedule(const drogon::HttpRequestPtr &req,
                                  std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                  const std::string &accountEmail,
                                  const std::string &leagueId) {
    const auto body = req->getJsonObject();
    int weeks = body && body->isMember("weeks") && (*body)["weeks"].isInt() ? (*body)["weeks"].asInt() : 12;
    weeks = std::clamp(weeks, 1, 15);
#ifdef CFF_HAS_POSTGRES
    if (dbConfigured()) {
        auto schedule = dbGenerateSeasonSchedule(accountEmail, leagueId, weeks);
        if (!schedule) {
            sendError(callback, drogon::k403Forbidden, "Only commissioners can regenerate an unlocked schedule");
            return;
        }
        callback(jsonResponse(*schedule, drogon::k200OK));
        return;
    }
#endif

    std::lock_guard<std::mutex> lock(storeMutex);
    if (!ensureCommissionerAccess(callback, accountEmail, leagueId, "Only commissioners can regenerate an unlocked schedule")) {
        return;
    }
    auto &matchups = arrayForLeague(matchupsByLeague, leagueId);
    for (const auto &matchup : matchups) {
        if (lowerString(jsonString(matchup, "status", "scheduled")) == "final") {
            sendError(callback, drogon::k409Conflict, "Cannot regenerate schedule after a week is final");
            return;
        }
    }
    const auto &members = arrayForLeague(membersByLeague, leagueId);
    const auto scoreForManager = [&accountEmail, &leagueId](const std::string &managerEmail) {
        if (managerEmail != accountEmail) return 0.0;
        double total = 0.0;
        for (const auto &player : arrayForLeague(rostersByLeague, leagueId)) {
            if (lowerString(jsonString(player, "rosterSlot", "bench")) != "bench") {
                total += projectionForPlayer(player);
            }
        }
        return total;
    };
    matchups = cff::league_schedule::buildSeasonSchedule(members, leagueId, weeks, scoreForManager);
    addTransactionLocked(leagueId, "Schedule", "Generated " + std::to_string(weeks) + "-week season schedule", accountEmail);
    callback(jsonResponse(matchups, drogon::k200OK));
}

void handleScoreWeek(const drogon::HttpRequestPtr &req,
                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                     const std::string &accountEmail,
                     const std::string &leagueId,
                     const std::string &week) {
    int weekNumber = 1;
    try {
        weekNumber = std::max(1, std::stoi(week));
    } catch (...) {
        sendError(callback, drogon::k400BadRequest, "Valid week is required");
        return;
    }
    const auto body = req->getJsonObject();
    const auto season = body && body->isMember("season") && (*body)["season"].isInt()
                            ? (*body)["season"].asInt()
                            : currentSeasonYear();
#ifdef CFF_HAS_POSTGRES
    if (dbConfigured()) {
        auto scored = dbScoreWeek(accountEmail, leagueId, season, weekNumber);
        if (!scored) {
            sendError(callback, drogon::k403Forbidden, "Only commissioners can score a league week");
            return;
        }
        if (scored->isObject() && scored->isMember("lineupErrors")) {
            callback(jsonResponse(*scored, drogon::k409Conflict));
            return;
        }
        callback(jsonResponse(*scored, drogon::k200OK));
        return;
    }
#endif

    std::lock_guard<std::mutex> lock(storeMutex);
    if (!ensureCommissionerAccess(callback, accountEmail, leagueId, "Only commissioners can score a league week")) {
        return;
    }
    const auto &members = arrayForLeague(membersByLeague, leagueId);
    const auto leagueIt = leaguesById.find(leagueId);
    const auto rules = leagueIt != leaguesById.end() ? leagueIt->second.league.rosterRules.toJson() : cff::RosterRules{}.toJson();
    std::unordered_map<std::string, int> counts;
    for (const auto &player : arrayForLeague(rostersByLeague, leagueId)) {
        const auto slot = lowerString(jsonString(player, "rosterSlot", "bench"));
        if (slot != "bench") counts[slot] += 1;
    }
    auto errors = cff::league_roster::lineupErrorsFromCounts(accountEmail, rules, counts);
    if (errors.size() > 0) {
        Json::Value payload;
        payload["error"] = "Invalid lineup";
        payload["lineupErrors"] = errors;
        callback(jsonResponse(payload, drogon::k409Conflict));
        return;
    }
    const auto scoreForManager = [&accountEmail, &leagueId](const std::string &managerEmail) {
        if (managerEmail != accountEmail) return 0.0;
        double total = 0.0;
        for (const auto &player : arrayForLeague(rostersByLeague, leagueId)) {
            if (lowerString(jsonString(player, "rosterSlot", "bench")) != "bench") {
                total += projectionForPlayer(player);
            }
        }
        return total;
    };
    auto &matchups = arrayForLeague(matchupsByLeague, leagueId);
    for (const auto &matchup : matchups) {
        if (cff::getIntOrDefault(matchup, "week", 1) == weekNumber && lowerString(jsonString(matchup, "status", "scheduled")) == "final") {
            sendError(callback, drogon::k409Conflict, "Week is already final");
            return;
        }
    }
    matchups = cff::league_schedule::buildMatchups(members, leagueId, weekNumber, scoreForManager);
    int matchupIndex = 1;
    for (auto &matchup : matchups) {
        matchup["week"] = weekNumber;
        matchup["id"] = leagueId + "-week-" + std::to_string(weekNumber) + "-" + std::to_string(matchupIndex++);
    }
    addTransactionLocked(leagueId, "Scoring", "Calculated week " + std::to_string(weekNumber) + " fantasy scores", accountEmail);
    Json::Value payload;
    payload["season"] = season;
    payload["week"] = weekNumber;
    payload["scores"] = Json::Value{Json::arrayValue};
    payload["matchups"] = matchups;
    callback(jsonResponse(payload, drogon::k200OK));
}

void handleFinalizeWeek(const drogon::HttpRequestPtr&,
                        std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                        const std::string &accountEmail,
                        const std::string &leagueId,
                        const std::string &week) {
    int weekNumber = 1;
    try {
        weekNumber = std::max(1, std::stoi(week));
    } catch (...) {
        sendError(callback, drogon::k400BadRequest, "Valid week is required");
        return;
    }
#ifdef CFF_HAS_POSTGRES
    if (dbConfigured()) {
        auto matchups = dbFinalizeWeek(accountEmail, leagueId, weekNumber);
        if (!matchups) {
            sendError(callback, drogon::k403Forbidden, "Only commissioners can finalize a scored week");
            return;
        }
        if (matchups->isObject() && matchups->isMember("lineupErrors")) {
            callback(jsonResponse(*matchups, drogon::k409Conflict));
            return;
        }
        callback(jsonResponse(*matchups, drogon::k200OK));
        return;
    }
#endif

    std::lock_guard<std::mutex> lock(storeMutex);
    if (!ensureCommissionerAccess(callback, accountEmail, leagueId, "Only commissioners can finalize a scored week")) {
        return;
    }
    auto &matchups = arrayForLeague(matchupsByLeague, leagueId);
    const auto leagueIt = leaguesById.find(leagueId);
    const auto rules = leagueIt != leaguesById.end() ? leagueIt->second.league.rosterRules.toJson() : cff::RosterRules{}.toJson();
    std::unordered_map<std::string, int> counts;
    for (const auto &player : arrayForLeague(rostersByLeague, leagueId)) {
        const auto slot = lowerString(jsonString(player, "rosterSlot", "bench"));
        if (slot != "bench") counts[slot] += 1;
    }
    auto errors = cff::league_roster::lineupErrorsFromCounts(accountEmail, rules, counts);
    if (errors.size() > 0) {
        Json::Value payload;
        payload["error"] = "Invalid lineup";
        payload["lineupErrors"] = errors;
        callback(jsonResponse(payload, drogon::k409Conflict));
        return;
    }
    if (!matchups.size()) {
        sendError(callback, drogon::k404NotFound, "No matchups found for this week");
        return;
    }
    const auto finalizedAt = timestampId("finalized");
    bool found = false;
    for (auto &matchup : matchups) {
        if (cff::getIntOrDefault(matchup, "week", 1) == weekNumber) {
            matchup["status"] = "final";
            matchup["finalizedAt"] = finalizedAt;
            found = true;
        }
    }
    if (!found) {
        sendError(callback, drogon::k404NotFound, "No matchups found for this week");
        return;
    }
    addTransactionLocked(leagueId, "Scoring Finalized", "Finalized week " + std::to_string(weekNumber), accountEmail);
    callback(jsonResponse(matchups, drogon::k200OK));
}

} // namespace cff::handlers
#endif // DROGON_FOUND
