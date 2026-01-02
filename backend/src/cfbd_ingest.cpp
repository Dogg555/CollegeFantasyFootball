#include "cfbd_ingest.h"

#include <algorithm>
#include <chrono>
#include <cpr/cpr.h>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <optional>
#include <pqxx/pqxx>
#include <sstream>

namespace {
std::optional<std::string> readEnv(const std::string &key) {
    const char *val = std::getenv(key.c_str());
    if (!val) {
        return std::nullopt;
    }
    return std::string{val};
}

std::string trimTrailingSlash(const std::string &url) {
    if (!url.empty() && url.back() == '/') {
        return url.substr(0, url.size() - 1);
    }
    return url;
}

std::string stringFromKeys(const nlohmann::json &j, std::initializer_list<const char*> keys) {
    for (auto key : keys) {
        if (j.contains(key) && !j.at(key).is_null()) {
            const auto &val = j.at(key);
            if (val.is_string()) {
                return val.get<std::string>();
            }
            if (val.is_number_integer()) {
                return std::to_string(val.get<long long>());
            }
            if (val.is_number_float()) {
                std::ostringstream oss;
                oss << std::setprecision(6) << val.get<double>();
                return oss.str();
            }
        }
    }
    return "";
}

std::optional<int> intFromKey(const nlohmann::json &j, const std::string &key) {
    if (j.contains(key) && j.at(key).is_number_integer()) {
        return j.at(key).get<int>();
    }
    return std::nullopt;
}

std::string currentYearString() {
    const auto now = std::chrono::system_clock::now();
    const auto tt = std::chrono::system_clock::to_time_t(now);
    const auto tm = *std::gmtime(&tt);
    return std::to_string(1900 + tm.tm_year);
}

void ensurePlayersSchema(pqxx::connection &conn) {
    pqxx::work tx{conn};
    tx.exec("CREATE EXTENSION IF NOT EXISTS pg_trgm;");
    tx.exec(R"SQL(
        CREATE TABLE IF NOT EXISTS players (
            id               TEXT PRIMARY KEY,
            full_name        TEXT NOT NULL,
            first_name       TEXT,
            last_name        TEXT,
            position         TEXT,
            team             TEXT,
            conference       TEXT,
            year             TEXT,
            height           TEXT,
            weight           INTEGER,
            updated_at       TIMESTAMPTZ NOT NULL DEFAULT NOW(),
            raw              JSONB
        );
    )SQL");
    tx.exec("CREATE INDEX IF NOT EXISTS idx_players_name_trgm ON players USING gin (full_name gin_trgm_ops);");
    tx.exec("CREATE INDEX IF NOT EXISTS idx_players_position ON players (position);");
    tx.exec("CREATE INDEX IF NOT EXISTS idx_players_conference ON players (conference);");
    tx.commit();
}

} // namespace

namespace cff {

std::vector<CfbdPlayer> fetchPlayersFromCFBD(const std::string &baseUrl,
                                             const std::string &apiKey,
                                             const std::string &season,
                                             int maxPages,
                                             std::vector<std::string> &errors,
                                             std::size_t &apiCalls) {
    std::vector<CfbdPlayer> players;
    const auto normalizedBase = trimTrailingSlash(baseUrl.empty() ? "https://api.collegefootballdata.com" : baseUrl);
    const auto endpoint = normalizedBase + "/players";

    const int cappedPages = std::max(1, std::min(maxPages, 500)); // guardrails for API quotas
    for (int page = 1; page <= cappedPages; ++page) {
        auto resp = cpr::Get(
            cpr::Url{endpoint},
            cpr::Header{{"Authorization", "Bearer " + apiKey}},
            cpr::Parameters{{"year", season}, {"page", std::to_string(page)}}
        );
        ++apiCalls;

        if (resp.error) {
            errors.push_back("Network error on page " + std::to_string(page) + ": " + resp.error.message);
            break;
        }

        if (resp.status_code == 401 || resp.status_code == 403) {
            errors.push_back("CFBD authentication failed (status " + std::to_string(resp.status_code) + ").");
            break;
        }

        if (resp.status_code >= 400) {
            errors.push_back("CFBD request failed for page " + std::to_string(page) +
                             " with status " + std::to_string(resp.status_code) + ".");
            break;
        }

        auto json = nlohmann::json::parse(resp.text, nullptr, false);
        if (!json.is_array()) {
            errors.push_back("Unexpected CFBD response shape for page " + std::to_string(page) + ".");
            break;
        }

        if (json.empty()) {
            break; // pagination complete
        }

        for (const auto &entry : json) {
            CfbdPlayer player;
            player.id = stringFromKeys(entry, {"id", "playerId", "athleteId"});
            if (player.id.empty()) {
                errors.push_back("Skipping player without id on page " + std::to_string(page));
                continue;
            }
            player.firstName = stringFromKeys(entry, {"first_name", "firstName"});
            player.lastName = stringFromKeys(entry, {"last_name", "lastName"});
            player.fullName = stringFromKeys(entry, {"full_name", "fullName"});
            if (player.fullName.empty()) {
                player.fullName = player.firstName + (player.firstName.empty() || player.lastName.empty() ? "" : " ") + player.lastName;
            }
            if (player.fullName.empty()) {
                player.fullName = "Player " + player.id;
            }
            player.position = stringFromKeys(entry, {"position"});
            player.team = stringFromKeys(entry, {"team", "school"});
            player.conference = stringFromKeys(entry, {"conference"});
            player.year = stringFromKeys(entry, {"year", "class"});
            player.height = stringFromKeys(entry, {"height"});
            player.weight = intFromKey(entry, "weight");
            player.raw = entry;
            players.push_back(std::move(player));
        }
    }

    return players;
}

IngestResult upsertPlayersToPostgres(const std::vector<CfbdPlayer> &players,
                                     const std::string &dbUrl,
                                     std::vector<std::string> &errors) {
    IngestResult result;
    if (players.empty()) {
        return result;
    }

    try {
        pqxx::connection conn{dbUrl};
        if (!conn.is_open()) {
            errors.push_back("Unable to connect to Postgres with DB_URL.");
            return result;
        }

        ensurePlayersSchema(conn);

        pqxx::work tx{conn};
        for (const auto &player : players) {
            const std::string weightValue = player.weight ? std::to_string(*player.weight) : "";
            auto dbResult = tx.exec_params(
                R"SQL(
                    INSERT INTO players (id, full_name, first_name, last_name, position, team, conference, year, height, weight, raw)
                    VALUES ($1, $2, NULLIF($3, ''), NULLIF($4, ''), NULLIF($5, ''), NULLIF($6, ''), NULLIF($7, ''), NULLIF($8, ''), NULLIF($9, ''), NULLIF($10, '')::INT, $11::jsonb)
                    ON CONFLICT (id) DO UPDATE SET
                        full_name = EXCLUDED.full_name,
                        first_name = EXCLUDED.first_name,
                        last_name = EXCLUDED.last_name,
                        position = EXCLUDED.position,
                        team = EXCLUDED.team,
                        conference = EXCLUDED.conference,
                        year = EXCLUDED.year,
                        height = EXCLUDED.height,
                        weight = EXCLUDED.weight,
                        raw = EXCLUDED.raw,
                        updated_at = NOW()
                    RETURNING (xmax = 0) AS inserted;
                )SQL",
                player.id,
                player.fullName,
                player.firstName,
                player.lastName,
                player.position,
                player.team,
                player.conference,
                player.year,
                player.height,
                weightValue,
                player.raw.dump()
            );

            if (!dbResult.empty() && dbResult[0][0].as<bool>()) {
                ++result.ingested;
            } else {
                ++result.updated;
            }
        }
        tx.commit();
    } catch (const std::exception &ex) {
        errors.push_back(std::string{"Postgres upsert failed: "} + ex.what());
    }

    return result;
}

IngestResult runCfbdIngestOnce() {
    IngestResult overall;
    const auto apiKey = readEnv("CFBD_API_KEY");
    if (!apiKey) {
        overall.errors.push_back("CFBD_API_KEY is required for ingestion.");
        return overall;
    }

    const auto dbUrl = readEnv("DB_URL");
    if (!dbUrl) {
        overall.errors.push_back("DB_URL is required for ingestion.");
        return overall;
    }

    const auto baseUrl = readEnv("CFBD_BASE_URL").value_or("https://api.collegefootballdata.com");
    const auto season = readEnv("CFBD_SEASON").value_or(currentYearString());
    int maxPages = 200;
    if (const auto envPages = readEnv("CFBD_MAX_PAGES")) {
        try {
            maxPages = std::stoi(*envPages);
        } catch (const std::exception &) {
            overall.errors.push_back("CFBD_MAX_PAGES is not a valid integer; using default 200.");
        }
    }

    std::vector<std::string> errors;
    std::size_t apiCalls = 0;
    const auto players = fetchPlayersFromCFBD(baseUrl, *apiKey, season, maxPages, errors, apiCalls);
    auto upserted = upsertPlayersToPostgres(players, *dbUrl, errors);

    overall.ingested = upserted.ingested;
    overall.updated = upserted.updated;
    overall.apiCalls = apiCalls;
    overall.errors = std::move(errors);
    return overall;
}

} // namespace cff
