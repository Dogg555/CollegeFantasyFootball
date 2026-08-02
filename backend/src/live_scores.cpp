#include "live_scores.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cpr/cpr.h>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <map>
#include <optional>
#include <pqxx/pqxx>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct ScheduleState {
    Json::Value games{Json::arrayValue};
    bool refresh{true};
};

std::optional<std::string> env(const char *name) {
    const char *value = std::getenv(name);
    if (!value || !*value) return std::nullopt;
    return std::string{value};
}

std::string trimSlash(std::string value) {
    while (!value.empty() && value.back() == '/') value.pop_back();
    return value;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

int currentSeason() {
    const auto now = std::chrono::system_clock::now();
    const auto raw = std::chrono::system_clock::to_time_t(now);
    const auto utc = *std::gmtime(&raw);
    int year = 1900 + utc.tm_year;
    return utc.tm_mon == 0 ? year - 1 : year;
}

int configuredSeason() {
    if (const auto value = env("CFBD_SEASON")) {
        try {
            const int parsed = std::stoi(*value);
            if (parsed >= 2000 && parsed <= 2100) return parsed;
        } catch (...) {
        }
    }
    return currentSeason();
}

int refreshHours() {
    if (const auto value = env("CFF_SCHEDULE_REFRESH_HOURS")) {
        try {
            return std::clamp(std::stoi(*value), 1, 24);
        } catch (...) {
        }
    }
    return 6;
}

std::string textAt(const Json::Value &value,
                   std::initializer_list<std::string> keys,
                   const std::string &fallback = "") {
    for (const auto &key : keys) {
        if (!value.isMember(key) || value[key].isNull()) continue;
        const auto &item = value[key];
        if (item.isString()) return item.asString();
        if (item.isInt64()) return std::to_string(item.asInt64());
        if (item.isUInt64()) return std::to_string(item.asUInt64());
        if (item.isInt()) return std::to_string(item.asInt());
        if (item.isUInt()) return std::to_string(item.asUInt());
    }
    return fallback;
}

int intAt(const Json::Value &value,
          std::initializer_list<std::string> keys,
          int fallback = 0) {
    for (const auto &key : keys) {
        if (!value.isMember(key) || value[key].isNull()) continue;
        const auto &item = value[key];
        if (item.isInt()) return item.asInt();
        if (item.isUInt()) return static_cast<int>(item.asUInt());
        if (item.isInt64()) return static_cast<int>(item.asInt64());
        if (item.isString()) {
            char *end = nullptr;
            const long parsed = std::strtol(item.asCString(), &end, 10);
            if (end != item.asCString()) return static_cast<int>(parsed);
        }
    }
    return fallback;
}

bool boolAt(const Json::Value &value,
            std::initializer_list<std::string> keys,
            bool fallback = false) {
    for (const auto &key : keys) {
        if (!value.isMember(key) || value[key].isNull()) continue;
        const auto &item = value[key];
        if (item.isBool()) return item.asBool();
        if (item.isInt()) return item.asInt() != 0;
        if (item.isString()) {
            const auto normalized = lower(item.asString());
            if (normalized == "true" || normalized == "1") return true;
            if (normalized == "false" || normalized == "0") return false;
        }
    }
    return fallback;
}

std::string teamAt(const Json::Value &game,
                   const std::string &side,
                   const std::string &fallback) {
    const auto direct = textAt(game, {side + "Team", side + "_team", side});
    if (!direct.empty()) return direct;
    for (const auto &key : {side + "Team", side}) {
        if (game.isMember(key) && game[key].isObject()) {
            return textAt(game[key], {"school", "name", "team"}, fallback);
        }
    }
    return fallback;
}

bool liveStatus(const std::string &status, int period) {
    const auto normalized = lower(status);
    if (normalized.find("final") != std::string::npos ||
        normalized.find("complete") != std::string::npos ||
        normalized.find("cancel") != std::string::npos ||
        normalized.find("postpon") != std::string::npos) return false;
    if (normalized.find("live") != std::string::npos ||
        normalized.find("progress") != std::string::npos ||
        normalized.find("half") != std::string::npos) return true;
    return period > 0 && normalized.find("scheduled") == std::string::npos;
}

std::string jsonText(const Json::Value &value) {
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    return Json::writeString(writer, value);
}

Json::Value parseJson(const std::string &text) {
    Json::CharReaderBuilder reader;
    Json::Value value;
    std::string errors;
    std::istringstream stream{text};
    if (!Json::parseFromStream(reader, stream, &value, &errors)) {
        return Json::Value(Json::arrayValue);
    }
    return value;
}

std::optional<Json::Value> fetchArray(const std::string &url,
                                      const std::string &apiKey,
                                      const cpr::Parameters &parameters,
                                      std::size_t &calls,
                                      std::string &error) {
    const auto response = cpr::Get(
        cpr::Url{url},
        cpr::Header{{"Authorization", "Bearer " + apiKey}},
        parameters,
        cpr::Timeout{60000}
    );
    ++calls;
    if (response.error || response.status_code < 200 || response.status_code >= 300) {
        error = "CFBD request to " + url + " failed with status " +
                std::to_string(response.status_code) + ": " + response.error.message;
        return std::nullopt;
    }
    auto parsed = parseJson(response.text);
    if (!parsed.isArray()) {
        error = "CFBD request to " + url + " did not return a JSON array.";
        return std::nullopt;
    }
    return parsed;
}

Json::Value normalizeGame(const Json::Value &game, bool scoreboard) {
    Json::Value cached;
    const int period = scoreboard ? intAt(game, {"period", "quarter"}) : 0;
    const bool complete = boolAt(game, {"completed"});
    const auto status = scoreboard
        ? textAt(game, {"status", "gameStatus"}, "scheduled")
        : (complete ? "final" : "scheduled");
    cached["id"] = textAt(game, {"id", "gameId"});
    cached["season"] = intAt(game, {"season", "year"});
    cached["week"] = intAt(game, {"week"});
    cached["seasonType"] = textAt(game, {"seasonType", "season_type"});
    cached["startDate"] = textAt(game, {"startDate", "start_date"});
    cached["away"] = teamAt(game, "away", "Away");
    cached["home"] = teamAt(game, "home", "Home");
    cached["awayScore"] = intAt(game, {"awayScore", "awayPoints", "away_score", "away_points"});
    cached["homeScore"] = intAt(game, {"homeScore", "homePoints", "home_score", "home_points"});
    cached["quarter"] = period;
    cached["clock"] = scoreboard ? textAt(game, {"clock", "displayClock"}) : "";
    cached["status"] = status;
    cached["live"] = scoreboard && liveStatus(status, period);
    cached["source"] = scoreboard ? "cfbd-scoreboard-cache" : "cfbd-schedule-cache";
    return cached;
}

Json::Value normalizeGames(const Json::Value &root, bool scoreboard) {
    Json::Value output(Json::arrayValue);
    for (const auto &game : root) {
        auto normalized = normalizeGame(game, scoreboard);
        if (!normalized["id"].asString().empty()) output.append(normalized);
    }
    return output;
}

Json::Value mergeGames(const Json::Value &schedule, const Json::Value &scoreboard) {
    std::map<std::string, Json::Value> byId;
    for (const auto &game : schedule) byId[textAt(game, {"id"})] = game;
    for (const auto &game : scoreboard) {
        const auto id = textAt(game, {"id"});
        if (id.empty()) continue;
        auto combined = byId.count(id) ? byId[id] : Json::Value(Json::objectValue);
        for (const auto &key : game.getMemberNames()) {
            const auto &value = game[key];
            if (!value.isNull() && !(value.isString() && value.asString().empty())) combined[key] = value;
        }
        byId[id] = combined;
    }
    std::vector<Json::Value> ordered;
    for (const auto &item : byId) if (!item.first.empty()) ordered.push_back(item.second);
    std::sort(ordered.begin(), ordered.end(), [](const auto &left, const auto &right) {
        const int leftWeek = intAt(left, {"week"});
        const int rightWeek = intAt(right, {"week"});
        if (leftWeek != rightWeek) return leftWeek < rightWeek;
        const auto leftDate = textAt(left, {"startDate"});
        const auto rightDate = textAt(right, {"startDate"});
        return leftDate != rightDate ? leftDate < rightDate : textAt(left, {"id"}) < textAt(right, {"id"});
    });
    Json::Value output(Json::arrayValue);
    for (const auto &game : ordered) output.append(game);
    return output;
}

ScheduleState loadSchedule(const std::string &dbUrl) {
    ScheduleState state;
    try {
        pqxx::connection connection{dbUrl};
        pqxx::read_transaction transaction{connection};
        const auto rows = transaction.exec(
            "SELECT schedule_payload::text, "
            "COALESCE(EXTRACT(EPOCH FROM (NOW() - schedule_fetched_at))::bigint, -1) "
            "FROM live_score_cache WHERE id = 1"
        );
        if (rows.empty()) return state;
        state.games = parseJson(rows[0][0].c_str());
        if (!state.games.isArray()) state.games = Json::Value(Json::arrayValue);
        const auto age = rows[0][1].as<long long>();
        state.refresh = state.games.empty() || age < 0 || age >= static_cast<long long>(refreshHours()) * 3600;
    } catch (const std::exception &error) {
        std::cerr << "[cfbd-live] schedule cache read failed: " << error.what() << std::endl;
    }
    return state;
}

void recordFailure(const std::string &dbUrl, const std::string &error, std::size_t calls) {
    try {
        pqxx::connection connection{dbUrl};
        pqxx::work transaction{connection};
        transaction.exec_params(
            "UPDATE live_score_cache SET status='failed', last_error=$1, updated_at=NOW() WHERE id=1",
            error
        );
        transaction.exec_params(
            "INSERT INTO ingestion_runs(resource, finished_at, status, call_count, row_count, error_message) "
            "VALUES('scoreboard', NOW(), 'failed', $1, 0, $2)",
            static_cast<int>(calls), error
        );
        transaction.commit();
    } catch (const std::exception &exception) {
        std::cerr << "[cfbd-live] unable to persist failure: " << exception.what() << std::endl;
    }
}

} // namespace

namespace cff {

LiveScoreIngestResult runLiveScoreIngestOnce() {
    LiveScoreIngestResult result;
    const auto apiKey = env("CFBD_API_KEY");
    const auto dbUrl = env("DB_URL");
    if (!apiKey) {
        result.errors.push_back("CFBD_API_KEY is required for live score ingestion.");
        return result;
    }
    if (!dbUrl) {
        result.errors.push_back("DB_URL is required for live score ingestion.");
        return result;
    }

    const auto baseUrl = trimSlash(env("CFBD_API_BASE_URL").value_or("https://api.collegefootballdata.com"));
    const int season = configuredSeason();
    auto scheduleState = loadSchedule(*dbUrl);

    std::string error;
    const auto scoreboardResponse = fetchArray(
        baseUrl + "/scoreboard", *apiKey,
        cpr::Parameters{{"classification", "fbs"}}, result.apiCalls, error
    );
    if (!scoreboardResponse) {
        result.errors.push_back(error);
        recordFailure(*dbUrl, error, result.apiCalls);
        return result;
    }
    const auto scoreboard = normalizeGames(*scoreboardResponse, true);

    Json::Value schedule = scheduleState.games;
    if (scheduleState.refresh) {
        const auto response = fetchArray(
            baseUrl + "/games", *apiKey,
            cpr::Parameters{{"year", std::to_string(season)}, {"seasonType", "both"}, {"classification", "fbs"}},
            result.apiCalls, error
        );
        if (response) {
            schedule = normalizeGames(*response, false);
            result.scheduleRefreshed = true;
        } else if (schedule.empty()) {
            result.errors.push_back(error);
        } else {
            std::cerr << "[cfbd-live] schedule refresh warning: " << error << std::endl;
        }
    }

    const auto payload = mergeGames(schedule, scoreboard);
    result.games = payload.size();
    result.scheduleGames = schedule.size();
    for (const auto &game : payload) if (boolAt(game, {"live"})) ++result.liveGames;

    try {
        pqxx::connection connection{*dbUrl};
        pqxx::work transaction{connection};
        if (result.scheduleRefreshed) {
            transaction.exec_params(
                "INSERT INTO live_score_cache(id,payload,fetched_at,status,last_error,game_count,live_game_count,"
                "schedule_payload,schedule_fetched_at,schedule_game_count,updated_at) "
                "VALUES(1,$1::jsonb,NOW(),$2,NULLIF($3,''),$4,$5,$6::jsonb,NOW(),$7,NOW()) "
                "ON CONFLICT(id) DO UPDATE SET payload=EXCLUDED.payload,fetched_at=NOW(),status=EXCLUDED.status,"
                "last_error=EXCLUDED.last_error,game_count=EXCLUDED.game_count,live_game_count=EXCLUDED.live_game_count,"
                "schedule_payload=EXCLUDED.schedule_payload,schedule_fetched_at=NOW(),"
                "schedule_game_count=EXCLUDED.schedule_game_count,updated_at=NOW()",
                jsonText(payload), result.errors.empty() ? "ok" : "failed",
                result.errors.empty() ? "" : result.errors.front(), static_cast<int>(result.games),
                static_cast<int>(result.liveGames), jsonText(schedule), static_cast<int>(result.scheduleGames)
            );
        } else {
            transaction.exec_params(
                "UPDATE live_score_cache SET payload=$1::jsonb,fetched_at=NOW(),status=$2,last_error=NULLIF($3,''),"
                "game_count=$4,live_game_count=$5,updated_at=NOW() WHERE id=1",
                jsonText(payload), result.errors.empty() ? "ok" : "failed",
                result.errors.empty() ? "" : result.errors.front(), static_cast<int>(result.games),
                static_cast<int>(result.liveGames)
            );
        }
        transaction.exec_params(
            "INSERT INTO ingestion_runs(resource,season,finished_at,status,call_count,row_count,error_message) "
            "VALUES('scoreboard',$1,NOW(),$2,$3,$4,NULLIF($5,''))",
            season, result.errors.empty() ? "success" : "failed", static_cast<int>(result.apiCalls),
            static_cast<int>(result.games), result.errors.empty() ? "" : result.errors.front()
        );
        transaction.commit();
    } catch (const std::exception &exception) {
        result.errors.push_back(std::string{"Unable to cache weekly scores: "} + exception.what());
    }
    return result;
}

Json::Value cachedLiveScorePayload() {
    const auto dbUrl = env("DB_URL");
    if (!dbUrl) return Json::Value(Json::arrayValue);
    try {
        pqxx::connection connection{*dbUrl};
        pqxx::read_transaction transaction{connection};
        const auto rows = transaction.exec("SELECT payload::text FROM live_score_cache WHERE id=1");
        if (rows.empty()) return Json::Value(Json::arrayValue);
        auto payload = parseJson(rows[0][0].c_str());
        return payload.isArray() ? payload : Json::Value(Json::arrayValue);
    } catch (const std::exception &error) {
        std::cerr << "[cfbd-live] cached score read failed: " << error.what() << std::endl;
        return Json::Value(Json::arrayValue);
    }
}

Json::Value cachedLiveScoreMeta() {
    Json::Value payload;
    const auto dbUrl = env("DB_URL");
    payload["databaseConfigured"] = dbUrl.has_value();
    if (!dbUrl) {
        payload["status"] = "unavailable";
        return payload;
    }
    try {
        pqxx::connection connection{*dbUrl};
        pqxx::read_transaction transaction{connection};
        const auto rows = transaction.exec(
            "SELECT status,COALESCE(fetched_at::text,''),game_count,live_game_count,"
            "COALESCE(EXTRACT(EPOCH FROM(NOW()-fetched_at))::bigint,-1),"
            "COALESCE(schedule_fetched_at::text,''),schedule_game_count,"
            "COALESCE(EXTRACT(EPOCH FROM(NOW()-schedule_fetched_at))::bigint,-1) "
            "FROM live_score_cache WHERE id=1"
        );
        if (rows.empty()) {
            payload["status"] = "never";
            payload["gameCount"] = 0;
            payload["liveGameCount"] = 0;
            payload["scheduleGameCount"] = 0;
            payload["fresh"] = false;
            payload["scheduleFresh"] = false;
            return payload;
        }
        const auto age = rows[0][4].as<long long>();
        const auto scheduleAge = rows[0][7].as<long long>();
        payload["status"] = rows[0][0].c_str();
        payload["fetchedAt"] = rows[0][1].c_str();
        payload["gameCount"] = rows[0][2].as<int>();
        payload["liveGameCount"] = rows[0][3].as<int>();
        payload["ageSeconds"] = static_cast<Json::Int64>(age);
        payload["scheduleFetchedAt"] = rows[0][5].c_str();
        payload["scheduleGameCount"] = rows[0][6].as<int>();
        payload["scheduleAgeSeconds"] = static_cast<Json::Int64>(scheduleAge);
        payload["fresh"] = age >= 0 && age <= 600;
        payload["scheduleFresh"] = scheduleAge >= 0 &&
            scheduleAge <= static_cast<long long>(refreshHours()) * 7200;
        return payload;
    } catch (const std::exception &error) {
        payload["status"] = "unavailable";
        return payload;
    }
}

Json::Value liveScoreIngestStatus() {
    Json::Value payload;
    const auto apiKey = env("CFBD_API_KEY");
    const auto dbUrl = env("DB_URL");
    payload["configured"] = apiKey.has_value();
    payload["databaseConfigured"] = dbUrl.has_value();
    payload["scheduleRefreshHours"] = refreshHours();
    if (!dbUrl) {
        payload["status"] = "unavailable";
        payload["error"] = "DB_URL is not configured.";
        return payload;
    }
    try {
        pqxx::connection connection{*dbUrl};
        pqxx::read_transaction transaction{connection};
        const auto rows = transaction.exec(
            "SELECT status,COALESCE(fetched_at::text,''),COALESCE(last_error,''),game_count,live_game_count,"
            "COALESCE(EXTRACT(EPOCH FROM(NOW()-fetched_at))::bigint,-1),"
            "(SELECT COALESCE(SUM(call_count),0) FROM ingestion_runs WHERE resource='scoreboard' "
            "AND started_at>=date_trunc('month',NOW())),COALESCE(schedule_fetched_at::text,''),"
            "schedule_game_count,COALESCE(EXTRACT(EPOCH FROM(NOW()-schedule_fetched_at))::bigint,-1) "
            "FROM live_score_cache WHERE id=1"
        );
        if (rows.empty()) {
            payload["status"] = "never";
            payload["gameCount"] = 0;
            payload["liveGameCount"] = 0;
            payload["scheduleGameCount"] = 0;
            payload["monthlyApiCalls"] = 0;
            return payload;
        }
        payload["status"] = rows[0][0].c_str();
        payload["fetchedAt"] = rows[0][1].c_str();
        payload["lastError"] = rows[0][2].c_str();
        payload["gameCount"] = rows[0][3].as<int>();
        payload["liveGameCount"] = rows[0][4].as<int>();
        payload["ageSeconds"] = static_cast<Json::Int64>(rows[0][5].as<long long>());
        payload["monthlyApiCalls"] = static_cast<Json::Int64>(rows[0][6].as<long long>());
        payload["scheduleFetchedAt"] = rows[0][7].c_str();
        payload["scheduleGameCount"] = rows[0][8].as<int>();
        payload["scheduleAgeSeconds"] = static_cast<Json::Int64>(rows[0][9].as<long long>());
        return payload;
    } catch (const std::exception &error) {
        payload["status"] = "unavailable";
        payload["error"] = error.what();
        return payload;
    }
}

} // namespace cff
