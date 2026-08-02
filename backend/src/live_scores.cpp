#include "live_scores.h"

#include <algorithm>
#include <cctype>
#include <cpr/cpr.h>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <pqxx/pqxx>
#include <sstream>

namespace {

std::optional<std::string> readEnv(const std::string &key) {
    const char *value = std::getenv(key.c_str());
    if (!value || std::string{value}.empty()) return std::nullopt;
    return std::string{value};
}

std::string trimTrailingSlash(std::string value) {
    while (!value.empty() && value.back() == '/') value.pop_back();
    return value;
}

std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string jsonStringAt(const Json::Value &value,
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

int jsonIntAt(const Json::Value &value,
              std::initializer_list<std::string> keys,
              int fallback = 0) {
    for (const auto &key : keys) {
        if (!value.isMember(key) || value[key].isNull()) continue;
        const auto &item = value[key];
        if (item.isInt()) return item.asInt();
        if (item.isUInt()) return static_cast<int>(item.asUInt());
        if (item.isString()) {
            char *end = nullptr;
            const auto parsed = std::strtol(item.asCString(), &end, 10);
            if (end != item.asCString()) return static_cast<int>(parsed);
        }
    }
    return fallback;
}

std::string teamNameFromSide(const Json::Value &game,
                             const std::string &side,
                             const std::string &fallback) {
    const auto direct = jsonStringAt(game, {side + "Team", side + "_team", side});
    if (!direct.empty()) return direct;
    if (game.isMember(side + "Team") && game[side + "Team"].isObject()) {
        return jsonStringAt(game[side + "Team"], {"school", "name", "team"}, fallback);
    }
    if (game.isMember(side) && game[side].isObject()) {
        return jsonStringAt(game[side], {"school", "name", "team"}, fallback);
    }
    return fallback;
}

bool isLiveGame(const std::string &status, int period) {
    const auto normalized = lowerAscii(status);
    if (normalized.find("final") != std::string::npos ||
        normalized.find("complete") != std::string::npos ||
        normalized.find("cancel") != std::string::npos ||
        normalized.find("postpon") != std::string::npos) {
        return false;
    }
    if (normalized.find("live") != std::string::npos ||
        normalized.find("progress") != std::string::npos ||
        normalized.find("halftime") != std::string::npos ||
        normalized.find("quarter") != std::string::npos) {
        return true;
    }
    return period > 0 && normalized.find("scheduled") == std::string::npos;
}

std::string jsonText(const Json::Value &value) {
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    return Json::writeString(writer, value);
}

Json::Value parseJson(const std::string &text) {
    Json::CharReaderBuilder reader;
    Json::Value parsed;
    std::string errors;
    std::istringstream stream{text};
    if (!Json::parseFromStream(reader, stream, &parsed, &errors)) {
        return Json::Value(Json::arrayValue);
    }
    return parsed;
}

void recordFailure(const std::string &dbUrl, const std::string &error, std::size_t apiCalls) {
    try {
        pqxx::connection conn{dbUrl};
        pqxx::work tx{conn};
        tx.exec_params(
            "UPDATE live_score_cache SET status = 'failed', last_error = $1, updated_at = NOW() WHERE id = 1",
            error
        );
        tx.exec_params(
            "INSERT INTO ingestion_runs (resource, finished_at, status, call_count, row_count, error_message) "
            "VALUES ('scoreboard', NOW(), 'failed', $1, 0, $2)",
            static_cast<int>(apiCalls),
            error
        );
        tx.commit();
    } catch (const std::exception &ex) {
        std::cerr << "[cfbd-live] unable to persist failed run: " << ex.what() << std::endl;
    }
}

} // namespace

namespace cff {

LiveScoreIngestResult runLiveScoreIngestOnce() {
    LiveScoreIngestResult result;
    const auto apiKey = readEnv("CFBD_API_KEY");
    const auto dbUrl = readEnv("DB_URL");
    if (!apiKey) {
        result.errors.push_back("CFBD_API_KEY is required for live score ingestion.");
        return result;
    }
    if (!dbUrl) {
        result.errors.push_back("DB_URL is required for live score ingestion.");
        return result;
    }

    const auto baseUrl = trimTrailingSlash(
        readEnv("CFBD_API_BASE_URL").value_or("https://api.collegefootballdata.com")
    );
    const auto response = cpr::Get(
        cpr::Url{baseUrl + "/scoreboard"},
        cpr::Header{{"Authorization", "Bearer " + *apiKey}},
        cpr::Parameters{{"classification", "fbs"}},
        cpr::Timeout{60000}
    );
    result.apiCalls = 1;

    if (response.error || response.status_code < 200 || response.status_code >= 300) {
        const auto error = "CFBD scoreboard request failed with status " +
                           std::to_string(response.status_code) + ": " + response.error.message;
        result.errors.push_back(error);
        recordFailure(*dbUrl, error, result.apiCalls);
        return result;
    }

    Json::CharReaderBuilder reader;
    Json::Value root;
    std::string parseErrors;
    std::istringstream body{response.text};
    if (!Json::parseFromStream(reader, body, &root, &parseErrors) || !root.isArray()) {
        const auto error = "CFBD scoreboard response was not a JSON array: " + parseErrors;
        result.errors.push_back(error);
        recordFailure(*dbUrl, error, result.apiCalls);
        return result;
    }

    Json::Value payload(Json::arrayValue);
    for (const auto &game : root) {
        Json::Value cached;
        const auto period = jsonIntAt(game, {"period", "quarter"}, 0);
        const auto status = jsonStringAt(game, {"status", "gameStatus"}, "scheduled");
        cached["id"] = jsonStringAt(game, {"id", "gameId"});
        cached["season"] = jsonIntAt(game, {"season", "year"}, 0);
        cached["week"] = jsonIntAt(game, {"week"}, 0);
        cached["startDate"] = jsonStringAt(game, {"startDate", "start_date"});
        cached["away"] = teamNameFromSide(game, "away", "Away");
        cached["home"] = teamNameFromSide(game, "home", "Home");
        cached["awayScore"] = jsonIntAt(game, {"awayScore", "awayPoints", "away_score", "away_points"});
        cached["homeScore"] = jsonIntAt(game, {"homeScore", "homePoints", "home_score", "home_points"});
        cached["quarter"] = period;
        cached["clock"] = jsonStringAt(game, {"clock", "displayClock"});
        cached["status"] = status;
        cached["live"] = isLiveGame(status, period);
        cached["source"] = "cfbd-cache";
        if (cached["live"].asBool()) ++result.liveGames;
        payload.append(cached);
    }
    result.games = payload.size();

    try {
        pqxx::connection conn{*dbUrl};
        pqxx::work tx{conn};
        tx.exec_params(
            "INSERT INTO live_score_cache (id, payload, fetched_at, status, last_error, game_count, live_game_count, updated_at) "
            "VALUES (1, $1::jsonb, NOW(), 'ok', NULL, $2, $3, NOW()) "
            "ON CONFLICT (id) DO UPDATE SET payload = EXCLUDED.payload, fetched_at = EXCLUDED.fetched_at, "
            "status = EXCLUDED.status, last_error = NULL, game_count = EXCLUDED.game_count, "
            "live_game_count = EXCLUDED.live_game_count, updated_at = NOW()",
            jsonText(payload),
            static_cast<int>(result.games),
            static_cast<int>(result.liveGames)
        );
        tx.exec_params(
            "INSERT INTO ingestion_runs (resource, finished_at, status, call_count, row_count) "
            "VALUES ('scoreboard', NOW(), 'success', $1, $2)",
            static_cast<int>(result.apiCalls),
            static_cast<int>(result.games)
        );
        tx.commit();
    } catch (const std::exception &ex) {
        result.errors.push_back(std::string{"Unable to cache live scores: "} + ex.what());
    }

    return result;
}

Json::Value cachedLiveScorePayload() {
    const auto dbUrl = readEnv("DB_URL");
    if (!dbUrl) return Json::Value(Json::arrayValue);
    try {
        pqxx::connection conn{*dbUrl};
        pqxx::read_transaction tx{conn};
        const auto rows = tx.exec("SELECT payload::text FROM live_score_cache WHERE id = 1");
        if (rows.empty()) return Json::Value(Json::arrayValue);
        auto payload = parseJson(rows[0][0].c_str());
        return payload.isArray() ? payload : Json::Value(Json::arrayValue);
    } catch (const std::exception &ex) {
        std::cerr << "[cfbd-live] unable to read cached scores: " << ex.what() << std::endl;
        return Json::Value(Json::arrayValue);
    }
}

Json::Value liveScoreIngestStatus() {
    Json::Value payload;
    const auto apiKey = readEnv("CFBD_API_KEY");
    const auto dbUrl = readEnv("DB_URL");
    payload["configured"] = apiKey.has_value();
    payload["databaseConfigured"] = dbUrl.has_value();
    if (!dbUrl) {
        payload["status"] = "unavailable";
        payload["error"] = "DB_URL is not configured.";
        return payload;
    }

    try {
        pqxx::connection conn{*dbUrl};
        pqxx::read_transaction tx{conn};
        const auto rows = tx.exec(
            "SELECT status, COALESCE(fetched_at::text, ''), COALESCE(last_error, ''), game_count, live_game_count, "
            "COALESCE(EXTRACT(EPOCH FROM (NOW() - fetched_at))::bigint, -1), "
            "(SELECT COALESCE(SUM(call_count), 0) FROM ingestion_runs "
            " WHERE resource = 'scoreboard' AND started_at >= date_trunc('month', NOW())) "
            "FROM live_score_cache WHERE id = 1"
        );
        if (rows.empty()) {
            payload["status"] = "never";
            payload["gameCount"] = 0;
            payload["liveGameCount"] = 0;
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
        return payload;
    } catch (const std::exception &ex) {
        payload["status"] = "unavailable";
        payload["error"] = ex.what();
        return payload;
    }
}

} // namespace cff
