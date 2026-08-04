#include "schedule_lineup_lifecycle.h"
#include "app_config.h"
#include "http_security.h"

#include <drogon/drogon.h>
#include <json/json.h>
#include <postgresql/libpq-fe.h>

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {
struct PgConnDeleter { void operator()(PGconn *value) const { if (value) PQfinish(value); } };
struct PgResultDeleter { void operator()(PGresult *value) const { if (value) PQclear(value); } };
using PgConn = std::unique_ptr<PGconn, PgConnDeleter>;
using PgResult = std::unique_ptr<PGresult, PgResultDeleter>;

std::string jsonText(const Json::Value &value) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, value);
}

Json::Value parseJson(const std::string &raw, Json::Value fallback = Json::Value{Json::objectValue}) {
    Json::CharReaderBuilder builder;
    std::istringstream stream(raw);
    std::string errors;
    Json::Value parsed;
    return Json::parseFromStream(builder, stream, &parsed, &errors) ? parsed : fallback;
}

PgConn connectDb() {
    const auto *url = std::getenv("DB_URL");
    if (!url || !*url) return nullptr;
    PgConn connection{PQconnectdb(url)};
    return connection && PQstatus(connection.get()) == CONNECTION_OK ? std::move(connection) : nullptr;
}

PgResult exec(PGconn *connection, const std::string &sql,
              const std::vector<std::string> &params = {}) {
    std::vector<const char *> values;
    values.reserve(params.size());
    for (const auto &param : params) values.push_back(param.c_str());
    return PgResult{PQexecParams(connection, sql.c_str(), static_cast<int>(params.size()),
                                 nullptr, values.data(), nullptr, nullptr, 0)};
}

bool tuples(PGresult *result) { return result && PQresultStatus(result) == PGRES_TUPLES_OK; }
bool command(PGresult *result) { return result && PQresultStatus(result) == PGRES_COMMAND_OK; }
std::string cell(PGresult *result, int row, int column) {
    return !result || PQgetisnull(result, row, column) ? "" : PQgetvalue(result, row, column);
}
long long number(PGresult *result, int row, int column, long long fallback = 0) {
    try { return std::stoll(cell(result, row, column)); } catch (...) { return fallback; }
}

std::vector<std::string> segments(const std::string &path) {
    std::vector<std::string> out;
    std::stringstream stream(path);
    std::string part;
    while (std::getline(stream, part, '/')) if (!part.empty()) out.push_back(part);
    return out;
}

std::optional<int> intValue(const std::string &value) {
    try { return std::stoi(value); } catch (...) { return std::nullopt; }
}

std::string operationKey(const drogon::HttpRequestPtr &request) {
    auto key = request->getHeader("Idempotency-Key");
    if (key.empty()) key = request->getHeader("X-Request-ID");
    return key;
}

long long expectedVersion(const Json::Value &body) {
    return body.isMember("expectedVersion") && body["expectedVersion"].isIntegral()
        ? body["expectedVersion"].asInt64() : -1;
}

void respond(const std::function<void(const drogon::HttpResponsePtr &)> &callback,
             Json::Value payload, drogon::HttpStatusCode status = drogon::k200OK) {
    auto response = drogon::HttpResponse::newHttpJsonResponse(payload);
    response->setStatusCode(status);
    callback(response);
}

void error(const std::function<void(const drogon::HttpResponsePtr &)> &callback,
           drogon::HttpStatusCode status, const std::string &code,
           const std::string &message, long long currentVersion = -1) {
    Json::Value payload(Json::objectValue);
    payload["error"] = message;
    payload["code"] = code;
    if (currentVersion >= 0) payload["currentVersion"] = Json::Int64(currentVersion);
    respond(callback, payload, status);
}

bool canAccess(PGconn *connection, const std::string &leagueId,
               const std::string &email, bool commissionerOnly = false) {
    auto result = exec(connection,
        "SELECT role FROM league_members WHERE league_id=$1 AND lower(email)=lower($2) AND status='active'",
        {leagueId, email});
    if (!tuples(result.get()) || PQntuples(result.get()) != 1) return false;
    return !commissionerOnly || cell(result.get(), 0, 0) == "commissioner";
}

Json::Value members(PGconn *connection, const std::string &leagueId) {
    auto result = exec(connection,
        "SELECT email, status FROM league_members WHERE league_id=$1 AND status='active' ORDER BY lower(email)",
        {leagueId});
    Json::Value list(Json::arrayValue);
    if (!tuples(result.get())) return list;
    for (int row = 0; row < PQntuples(result.get()); ++row) {
        Json::Value item(Json::objectValue);
        item["email"] = cell(result.get(), row, 0);
        item["status"] = cell(result.get(), row, 1);
        list.append(item);
    }
    return list;
}

long long currentScheduleVersion(PGconn *connection, const std::string &leagueId, int season) {
    auto result = exec(connection,
        "SELECT version FROM league_schedule_states WHERE league_id=$1 AND season=$2::int",
        {leagueId, std::to_string(season)});
    return tuples(result.get()) && PQntuples(result.get()) ? number(result.get(), 0, 0) : 0;
}

Json::Value scheduleState(PGconn *connection, const std::string &leagueId, int season) {
    Json::Value payload(Json::objectValue);
    payload["leagueId"] = leagueId;
    payload["season"] = season;
    auto state = exec(connection,
        "SELECT version,weeks,schedule_hash,COALESCE(to_char(generated_at AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'),'') "
        "FROM league_schedule_states WHERE league_id=$1 AND season=$2::int",
        {leagueId, std::to_string(season)});
    payload["version"] = Json::Int64(tuples(state.get()) && PQntuples(state.get()) ? number(state.get(), 0, 0) : 0);
    payload["weeks"] = tuples(state.get()) && PQntuples(state.get()) ? static_cast<int>(number(state.get(), 0, 1)) : 0;
    payload["scheduleHash"] = tuples(state.get()) && PQntuples(state.get()) ? cell(state.get(), 0, 2) : "";
    payload["generatedAt"] = tuples(state.get()) && PQntuples(state.get()) ? cell(state.get(), 0, 3) : "";

    auto matchups = exec(connection,
        "SELECT id,week,home_manager_email,COALESCE(away_manager_email,''),status,identity_key,schedule_version "
        "FROM league_matchups WHERE league_id=$1 AND season=$2::int ORDER BY week,id",
        {leagueId, std::to_string(season)});
    payload["matchups"] = Json::Value{Json::arrayValue};
    if (tuples(matchups.get())) for (int row=0; row<PQntuples(matchups.get()); ++row) {
        Json::Value item(Json::objectValue);
        item["id"] = cell(matchups.get(), row, 0);
        item["week"] = static_cast<int>(number(matchups.get(), row, 1));
        item["homeManager"] = cell(matchups.get(), row, 2);
        item["awayManager"] = cell(matchups.get(), row, 3);
        item["status"] = cell(matchups.get(), row, 4);
        item["identityKey"] = cell(matchups.get(), row, 5);
        item["scheduleVersion"] = Json::Int64(number(matchups.get(), row, 6));
        payload["matchups"].append(item);
    }

    auto weeks = exec(connection,
        "SELECT week,version,status,COALESCE(to_char(lineup_deadline AT TIME ZONE 'UTC','YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'),''),locked_by_email "
        "FROM lineup_week_states WHERE league_id=$1 AND season=$2::int ORDER BY week",
        {leagueId, std::to_string(season)});
    payload["lineupWeeks"] = Json::Value{Json::arrayValue};
    if (tuples(weeks.get())) for (int row=0; row<PQntuples(weeks.get()); ++row) {
        Json::Value item(Json::objectValue);
        item["week"] = static_cast<int>(number(weeks.get(), row, 0));
        item["version"] = Json::Int64(number(weeks.get(), row, 1));
        item["status"] = cell(weeks.get(), row, 2);
        item["lineupDeadline"] = cell(weeks.get(), row, 3);
        item["lockedByEmail"] = cell(weeks.get(), row, 4);
        item["locked"] = item["status"].asString() != "open";
        payload["lineupWeeks"].append(item);
    }
    return payload;
}

bool replay(PGconn *connection, const std::string &leagueId, const std::string &email,
            const std::string &key, Json::Value &payload) {
    if (key.empty()) return false;
    auto result = exec(connection,
        "SELECT response_payload::text FROM schedule_lineup_operations WHERE league_id=$1 AND lower(actor_email)=lower($2) AND operation_key=$3",
        {leagueId, email, key});
    if (!tuples(result.get()) || PQntuples(result.get()) != 1) return false;
    payload = parseJson(cell(result.get(), 0, 0));
    payload["idempotentReplay"] = true;
    return true;
}

bool saveOperation(PGconn *connection, const std::string &leagueId, const std::string &email,
                   const std::string &key, const std::string &type, long long version,
                   const Json::Value &payload) {
    if (key.empty()) return true;
    auto result = exec(connection,
        "INSERT INTO schedule_lineup_operations(league_id,actor_email,operation_key,operation_type,resulting_version,response_payload) "
        "VALUES($1,lower($2),$3,$4,$5::bigint,$6::jsonb) ON CONFLICT DO NOTHING",
        {leagueId,email,key,type,std::to_string(version),jsonText(payload)});
    return command(result.get());
}

bool beginLocked(PGconn *connection, const std::string &leagueId) {
    auto begin = exec(connection, "BEGIN");
    if (!command(begin.get())) return false;
    auto lock = exec(connection, "SELECT pg_advisory_xact_lock(hashtextextended($1,0))", {"schedule-lineup:" + leagueId});
    return tuples(lock.get());
}

bool commit(PGconn *connection) { return command(exec(connection, "COMMIT").get()); }
void rollback(PGconn *connection) { (void)exec(connection, "ROLLBACK"); }

void generateSchedule(const drogon::HttpRequestPtr &request,
                      const std::function<void(const drogon::HttpResponsePtr &)> &callback,
                      const std::string &email, const std::string &leagueId,
                      int season, const Json::Value &body) {
    auto connection = connectDb();
    if (!connection || !canAccess(connection.get(), leagueId, email, true)) {
        error(callback, drogon::k403Forbidden, "commissioner_required", "Commissioner access is required"); return;
    }
    if (!beginLocked(connection.get(), leagueId)) { error(callback,drogon::k500InternalServerError,"schedule_lock_failed","Unable to lock schedule"); return; }
    Json::Value cached;
    const auto key = operationKey(request);
    if (replay(connection.get(), leagueId, email, key, cached)) { commit(connection.get()); respond(callback,cached); return; }
    const auto current = currentScheduleVersion(connection.get(), leagueId, season);
    const auto expected = expectedVersion(body);
    if (expected < 0) { rollback(connection.get()); error(callback,drogon::k428PreconditionRequired,"schedule_precondition_required","expectedVersion is required",current); return; }
    if (expected != current) { rollback(connection.get()); error(callback,drogon::k409Conflict,"schedule_state_conflict","Schedule state is stale",current); return; }
    auto immutable = exec(connection,
        "SELECT COUNT(*) FROM league_matchups WHERE league_id=$1 AND season=$2::int AND status='final'",
        {leagueId,std::to_string(season)});
    if (!tuples(immutable.get()) || number(immutable.get(),0,0)>0) { rollback(connection.get()); error(callback,drogon::k409Conflict,"schedule_immutable","Cannot regenerate after a finalized matchup",current); return; }
    const int weeks = std::clamp(body.get("weeks",12).asInt(),1,15);
    const auto schedule = cff::schedule_lineup::deterministicSchedule(members(connection.get(),leagueId),leagueId,season,weeks);
    const auto next = cff::schedule_lineup::nextVersion(current);
    const auto hash = std::to_string(std::hash<std::string>{}(jsonText(schedule)));
    if (!command(exec(connection.get(),"DELETE FROM league_matchups WHERE league_id=$1 AND season=$2::int",{leagueId,std::to_string(season)}).get())) { rollback(connection.get()); error(callback,drogon::k500InternalServerError,"schedule_write_failed","Unable to replace schedule"); return; }
    for (const auto &matchup : schedule) {
        auto inserted = exec(connection.get(),
            "INSERT INTO league_matchups(id,league_id,season,week,home_manager_email,away_manager_email,status,schedule_version,identity_key) "
            "VALUES($1,$2,$3::int,$4::int,$5,NULLIF($6,''),'scheduled',$7::bigint,$8)",
            {matchup["id"].asString(),leagueId,std::to_string(season),std::to_string(matchup["week"].asInt()),
             matchup["homeManager"].asString(),matchup["awayManager"].asString(),std::to_string(next),matchup["id"].asString()});
        if (!command(inserted.get())) { rollback(connection.get()); error(callback,drogon::k500InternalServerError,"schedule_write_failed","Unable to persist schedule"); return; }
    }
    auto state = exec(connection.get(),
        "INSERT INTO league_schedule_states(league_id,season,version,weeks,schedule_hash,generated_by_email,generated_at,updated_at) "
        "VALUES($1,$2::int,$3::bigint,$4::int,$5,lower($6),NOW(),NOW()) "
        "ON CONFLICT(league_id) DO UPDATE SET season=EXCLUDED.season,version=EXCLUDED.version,weeks=EXCLUDED.weeks,schedule_hash=EXCLUDED.schedule_hash,generated_by_email=EXCLUDED.generated_by_email,generated_at=NOW(),updated_at=NOW()",
        {leagueId,std::to_string(season),std::to_string(next),std::to_string(weeks),hash,email});
    if (!command(state.get())) { rollback(connection.get()); error(callback,drogon::k500InternalServerError,"schedule_write_failed","Unable to save schedule state"); return; }
    auto weekRows = exec(connection.get(),
        "INSERT INTO lineup_week_states(league_id,season,week,version,status,updated_at) "
        "SELECT $1,$2::int,generate_series(1,$3::int),0,'open',NOW() ON CONFLICT(league_id,season,week) DO NOTHING",
        {leagueId,std::to_string(season),std::to_string(weeks)});
    if (!command(weekRows.get())) { rollback(connection.get()); error(callback,drogon::k500InternalServerError,"lineup_state_failed","Unable to initialize lineup weeks"); return; }
    auto payload = scheduleState(connection.get(),leagueId,season);
    payload["operation"] = "generate";
    saveOperation(connection.get(),leagueId,email,key,"generate_schedule",next,payload);
    if (!commit(connection.get())) { error(callback,drogon::k500InternalServerError,"schedule_commit_failed","Unable to commit schedule"); return; }
    respond(callback,payload);
}

void changeLock(const drogon::HttpRequestPtr &request,
                const std::function<void(const drogon::HttpResponsePtr &)> &callback,
                const std::string &email, const std::string &leagueId,
                int season, int week, bool lockWeek, const Json::Value &body) {
    auto connection = connectDb();
    if (!connection || !canAccess(connection.get(),leagueId,email,true)) { error(callback,drogon::k403Forbidden,"commissioner_required","Commissioner access is required"); return; }
    if (!beginLocked(connection.get(),leagueId)) { error(callback,drogon::k500InternalServerError,"lineup_lock_failed","Unable to lock lineup state"); return; }
    const auto key=operationKey(request); Json::Value cached;
    if (replay(connection.get(),leagueId,email,key,cached)) { commit(connection.get()); respond(callback,cached); return; }
    auto currentRow=exec(connection.get(),"SELECT version,status FROM lineup_week_states WHERE league_id=$1 AND season=$2::int AND week=$3::int FOR UPDATE",{leagueId,std::to_string(season),std::to_string(week)});
    if (!tuples(currentRow.get()) || PQntuples(currentRow.get())!=1) { rollback(connection.get()); error(callback,drogon::k404NotFound,"lineup_week_not_found","Lineup week was not found"); return; }
    const auto current=number(currentRow.get(),0,0); const auto expected=expectedVersion(body);
    if (expected<0) { rollback(connection.get()); error(callback,drogon::k428PreconditionRequired,"lineup_precondition_required","expectedVersion is required",current); return; }
    if (expected!=current) { rollback(connection.get()); error(callback,drogon::k409Conflict,"lineup_state_conflict","Lineup state is stale",current); return; }
    if (!lockWeek && cell(currentRow.get(),0,1)=="final") { rollback(connection.get()); error(callback,drogon::k409Conflict,"lineup_finalized","A finalized week cannot be unlocked",current); return; }
    const auto next=cff::schedule_lineup::nextVersion(current);
    const auto deadline=body.get("lineupDeadline","").asString();
    auto update=exec(connection.get(),
        lockWeek
        ? "UPDATE lineup_week_states SET version=$4::bigint,status='locked',lineup_deadline=COALESCE(NULLIF($5,'')::timestamptz,lineup_deadline),locked_at=NOW(),locked_by_email=lower($6),updated_at=NOW() WHERE league_id=$1 AND season=$2::int AND week=$3::int"
        : "UPDATE lineup_week_states SET version=$4::bigint,status='open',locked_at=NULL,locked_by_email='',updated_at=NOW() WHERE league_id=$1 AND season=$2::int AND week=$3::int",
        lockWeek ? std::vector<std::string>{leagueId,std::to_string(season),std::to_string(week),std::to_string(next),deadline,email}
                 : std::vector<std::string>{leagueId,std::to_string(season),std::to_string(week),std::to_string(next)});
    if (!command(update.get())) { rollback(connection.get()); error(callback,drogon::k500InternalServerError,"lineup_state_failed","Unable to update lineup state"); return; }
    if (lockWeek) {
        auto snapshot=exec(connection.get(),
            "INSERT INTO lineup_snapshots(league_id,season,week,manager_email,lineup_version,roster_revision,lineup) "
            "SELECT $1,$2::int,$3::int,lm.email,$4::bigint,COALESCE(rs.version,0),COALESCE(jsonb_agg(jsonb_build_object('playerId',r.player_id,'rosterSlot',r.roster_slot,'player',r.player_snapshot) ORDER BY r.roster_slot,r.player_id) FILTER(WHERE r.player_id IS NOT NULL),'[]'::jsonb) "
            "FROM league_members lm LEFT JOIN roster_states rs ON rs.league_id=lm.league_id AND lower(rs.manager_email)=lower(lm.email) LEFT JOIN rosters r ON r.league_id=lm.league_id AND lower(r.manager_email)=lower(lm.email) "
            "WHERE lm.league_id=$1 AND lm.status='active' GROUP BY lm.email,rs.version ON CONFLICT DO NOTHING",
            {leagueId,std::to_string(season),std::to_string(week),std::to_string(next)});
        if (!command(snapshot.get())) { rollback(connection.get()); error(callback,drogon::k500InternalServerError,"lineup_snapshot_failed","Unable to capture lineups"); return; }
    }
    auto payload=scheduleState(connection.get(),leagueId,season);
    payload["operation"]=lockWeek?"lock":"unlock"; payload["week"]=week; payload["lineupVersion"]=Json::Int64(next);
    saveOperation(connection.get(),leagueId,email,key,lockWeek?"lock_week":"unlock_week",next,payload);
    if (!commit(connection.get())) { error(callback,drogon::k500InternalServerError,"lineup_commit_failed","Unable to commit lineup state"); return; }
    respond(callback,payload);
}

struct Install {
    Install() {
        drogon::app().registerSyncAdvice([](const drogon::HttpRequestPtr &request) -> drogon::HttpResponsePtr {
            const auto parts=segments(request->path());
            if (parts.size()<4 || parts[0]!="api" || parts[1]!="leagues") return nullptr;
            const auto leagueId=parts[2];
            const auto config=cff::config::loadRuntimeConfig();
            auto email=cff::http::accountEmailForRequest(request,config.jwtSecret);
            if (!email) return nullptr;
            const auto body=request->getJsonObject()?*request->getJsonObject():Json::Value{Json::objectValue};
            const int season=body.get("season",2026).asInt();
            drogon::HttpResponsePtr output;
            auto callback=[&output](const drogon::HttpResponsePtr &value){ output=value; };
            if (parts.size()==5 && parts[3]=="schedule" && parts[4]=="state" && request->method()==drogon::Get) {
                auto connection=connectDb(); if (!connection || !canAccess(connection.get(),leagueId,*email)) return nullptr;
                respond(callback,scheduleState(connection.get(),leagueId,season)); return output;
            }
            if (parts.size()==5 && parts[3]=="schedule" && parts[4]=="generate" && request->method()==drogon::Post) {
                generateSchedule(request,callback,*email,leagueId,season,body); return output;
            }
            if (parts.size()==7 && parts[3]=="lineups" && parts[4]=="week") {
                const auto week=intValue(parts[5]); if (!week) return nullptr;
                if (parts[6]=="lock" && request->method()==drogon::Post) { changeLock(request,callback,*email,leagueId,season,*week,true,body); return output; }
                if (parts[6]=="unlock" && request->method()==drogon::Post) { changeLock(request,callback,*email,leagueId,season,*week,false,body); return output; }
            }
            return nullptr;
        });
    }
} install;
}
