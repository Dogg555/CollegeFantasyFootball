#include "cfbd_ingest.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cpr/cpr.h>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <optional>
#include <pqxx/pqxx>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace {

struct CfbdTeam {
    std::string school;
    std::string conference;
};

struct CfbdQuota {
    std::optional<long long> remainingCalls;
    std::string resetAt;
};

struct JsonRequestResult {
    nlohmann::json payload;
    bool ok = false;
    bool rateLimited = false;
};

std::optional<std::string> readEnv(const std::string &key) {
    const char *value = std::getenv(key.c_str());
    if (!value || std::string{value}.empty()) return std::nullopt;
    return std::string{value};
}

std::string trimTrailingSlash(std::string value) {
    while (!value.empty() && value.back() == '/') value.pop_back();
    return value;
}

std::string stringFromKeys(const nlohmann::json &value,
                           std::initializer_list<const char*> keys) {
    for (const auto *key : keys) {
        if (!value.contains(key) || value.at(key).is_null()) continue;
        const auto &item = value.at(key);
        if (item.is_string()) return item.get<std::string>();
        if (item.is_number_integer()) return std::to_string(item.get<long long>());
        if (item.is_number_unsigned()) return std::to_string(item.get<unsigned long long>());
        if (item.is_number_float()) {
            std::ostringstream output;
            output << std::setprecision(8) << item.get<double>();
            return output.str();
        }
    }
    return "";
}

std::optional<int> intFromKeys(const nlohmann::json &value,
                               std::initializer_list<const char*> keys) {
    for (const auto *key : keys) {
        if (!value.contains(key) || value.at(key).is_null()) continue;
        const auto &item = value.at(key);
        if (item.is_number_integer()) return item.get<int>();
        if (item.is_number_unsigned()) return static_cast<int>(item.get<unsigned int>());
        if (item.is_string()) {
            char *end = nullptr;
            const auto raw = item.get<std::string>();
            const long parsed = std::strtol(raw.c_str(), &end, 10);
            if (end != raw.c_str()) return static_cast<int>(parsed);
        }
    }
    return std::nullopt;
}

std::optional<long long> longLongFromKeys(const nlohmann::json &value,
                                          std::initializer_list<const char*> keys) {
    for (const auto *key : keys) {
        if (!value.contains(key) || value.at(key).is_null()) continue;
        const auto &item = value.at(key);
        if (item.is_number_integer()) return item.get<long long>();
        if (item.is_number_unsigned()) return static_cast<long long>(item.get<unsigned long long>());
        if (item.is_number_float()) return static_cast<long long>(item.get<double>());
        if (item.is_string()) {
            char *end = nullptr;
            const auto raw = item.get<std::string>();
            const long long parsed = std::strtoll(raw.c_str(), &end, 10);
            if (end != raw.c_str()) return parsed;
        }
    }
    return std::nullopt;
}

std::string currentYearString() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    const auto utc = *std::gmtime(&time);
    return std::to_string(1900 + utc.tm_year);
}

int parseSeason(const std::string &season, std::vector<std::string> &errors) {
    try {
        const int parsed = std::stoi(season);
        if (parsed >= 2000 && parsed <= 2100) return parsed;
    } catch (const std::exception &) {
    }
    errors.push_back("CFBD_SEASON must be a four-digit season year.");
    return 0;
}

int configuredMaxTeams(std::vector<std::string> &errors) {
    const auto configured = readEnv("CFBD_MAX_TEAMS").value_or(
        readEnv("CFBD_MAX_PAGES").value_or("200")
    );
    try {
        return std::max(1, std::min(std::stoi(configured), 200));
    } catch (const std::exception &) {
        errors.push_back("CFBD_MAX_TEAMS is not a valid integer; using 200.");
        return 200;
    }
}

std::string responseHeader(const cpr::Response &response, const std::string &name) {
    auto header = response.header.find(name);
    if (header != response.header.end()) return header->second;

    std::string lowered = name;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    for (const auto &entry : response.header) {
        auto key = entry.first;
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        if (key == lowered) return entry.second;
    }
    return "";
}

std::string responseDetail(const cpr::Response &response) {
    const auto payload = nlohmann::json::parse(response.text, nullptr, false);
    if (!payload.is_object()) return "";
    return stringFromKeys(payload, {"message", "error", "detail"});
}

JsonRequestResult requestJson(const std::string &url,
                              const std::string &apiKey,
                              const cpr::Parameters &parameters,
                              const std::string &label,
                              std::vector<std::string> &errors,
                              std::size_t &apiCalls) {
    const auto response = cpr::Get(
        cpr::Url{url},
        cpr::Header{{"Authorization", "Bearer " + apiKey}},
        parameters,
        cpr::Timeout{60000}
    );
    ++apiCalls;

    JsonRequestResult result;
    if (response.error) {
        errors.push_back(label + " network error: " + response.error.message);
        return result;
    }
    if (response.status_code == 401 || response.status_code == 403) {
        errors.push_back(label + " authentication failed with status " +
                         std::to_string(response.status_code) + ".");
        return result;
    }
    if (response.status_code == 429) {
        std::string message = label + " was rate-limited by CFBD (429)";
        const auto detail = responseDetail(response);
        const auto retryAfter = responseHeader(response, "Retry-After");
        if (!detail.empty()) message += ": " + detail;
        if (!retryAfter.empty()) message += "; retry after " + retryAfter;
        message += ". The roster refresh stopped immediately and the existing player catalog was preserved.";
        errors.push_back(std::move(message));
        result.rateLimited = true;
        return result;
    }
    if (response.status_code < 200 || response.status_code >= 300) {
        std::string message = label + " failed with status " +
                              std::to_string(response.status_code);
        const auto detail = responseDetail(response);
        if (!detail.empty()) message += ": " + detail;
        errors.push_back(message + ".");
        return result;
    }

    result.payload = nlohmann::json::parse(response.text, nullptr, false);
    if (result.payload.is_discarded()) {
        errors.push_back(label + " returned invalid JSON.");
        return result;
    }
    result.ok = true;
    return result;
}

std::optional<CfbdQuota> fetchQuota(const std::string &baseUrl,
                                    const std::string &apiKey,
                                    std::vector<std::string> &errors,
                                    std::size_t &apiCalls) {
    const auto response = requestJson(
        baseUrl + "/info",
        apiKey,
        cpr::Parameters{},
        "CFBD quota preflight",
        errors,
        apiCalls
    );
    if (!response.ok) return std::nullopt;
    if (!response.payload.is_object()) {
        errors.push_back("CFBD quota preflight returned an unexpected response shape.");
        return std::nullopt;
    }

    CfbdQuota quota;
    quota.remainingCalls = longLongFromKeys(
        response.payload,
        {"remainingCalls", "remaining_calls"}
    );
    quota.resetAt = stringFromKeys(response.payload, {"resetAt", "reset_at"});
    if (!quota.remainingCalls) {
        errors.push_back("CFBD quota preflight did not include remainingCalls.");
        return std::nullopt;
    }
    return quota;
}

std::vector<CfbdTeam> fetchFbsTeams(const std::string &baseUrl,
                                    const std::string &apiKey,
                                    const std::string &season,
                                    std::vector<std::string> &errors,
                                    std::size_t &apiCalls) {
    const auto response = requestJson(
        baseUrl + "/teams/fbs",
        apiKey,
        cpr::Parameters{{"year", season}},
        "CFBD FBS team list",
        errors,
        apiCalls
    );
    if (!response.ok) return {};
    if (!response.payload.is_array()) {
        errors.push_back("CFBD FBS team list returned an unexpected response shape.");
        return {};
    }

    std::vector<CfbdTeam> teams;
    teams.reserve(response.payload.size());
    for (const auto &entry : response.payload) {
        CfbdTeam team;
        team.school = stringFromKeys(entry, {"school", "name"});
        team.conference = stringFromKeys(entry, {"conference"});
        if (team.school.empty()) continue;
        teams.push_back(std::move(team));
    }
    std::sort(teams.begin(), teams.end(), [](const auto &left, const auto &right) {
        return left.school < right.school;
    });
    return teams;
}

void ensurePlayersSchema(pqxx::connection &connection) {
    pqxx::work transaction{connection};
    transaction.exec("CREATE EXTENSION IF NOT EXISTS pg_trgm;");
    transaction.exec(R"SQL(
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
            season           INTEGER,
            active           BOOLEAN NOT NULL DEFAULT TRUE,
            last_seen_at     TIMESTAMPTZ NOT NULL DEFAULT NOW(),
            updated_at       TIMESTAMPTZ NOT NULL DEFAULT NOW(),
            raw              JSONB
        );
    )SQL");
    transaction.exec("ALTER TABLE players ADD COLUMN IF NOT EXISTS season INTEGER;");
    transaction.exec("ALTER TABLE players ADD COLUMN IF NOT EXISTS active BOOLEAN NOT NULL DEFAULT TRUE;");
    transaction.exec("ALTER TABLE players ADD COLUMN IF NOT EXISTS last_seen_at TIMESTAMPTZ NOT NULL DEFAULT NOW();");
    transaction.exec("CREATE INDEX IF NOT EXISTS idx_players_name_trgm ON players USING gin (full_name gin_trgm_ops);");
    transaction.exec("CREATE INDEX IF NOT EXISTS idx_players_position ON players (position);");
    transaction.exec("CREATE INDEX IF NOT EXISTS idx_players_conference ON players (conference);");
    transaction.exec("CREATE INDEX IF NOT EXISTS idx_players_active_season ON players (active, season DESC);");
    transaction.exec("CREATE INDEX IF NOT EXISTS idx_players_team_active ON players (team, active);");
    transaction.commit();
}

std::string joinErrors(const std::vector<std::string> &errors) {
    std::ostringstream output;
    for (std::size_t index = 0; index < errors.size(); ++index) {
        if (index > 0) output << " | ";
        output << errors[index];
    }
    auto text = output.str();
    constexpr std::size_t kMaxErrorLength = 4000;
    if (text.size() > kMaxErrorLength) text.resize(kMaxErrorLength);
    return text;
}

void recordIngestionRun(const std::string &dbUrl,
                        int season,
                        const cff::IngestResult &result) {
    try {
        pqxx::connection connection{dbUrl};
        pqxx::work transaction{connection};
        const auto status = result.complete && result.errors.empty() ? "success" : "partial";
        transaction.exec_params(
            "INSERT INTO ingestion_runs (resource, season, finished_at, status, call_count, row_count, error_message) "
            "VALUES ('players', $1, NOW(), $2, $3, $4, NULLIF($5, ''))",
            season,
            status,
            static_cast<int>(result.apiCalls),
            static_cast<int>(result.ingested + result.updated),
            joinErrors(result.errors)
        );
        transaction.commit();
    } catch (const std::exception &error) {
        std::cerr << "[cfbd] unable to record player ingestion run: " << error.what() << std::endl;
    }
}

} // namespace

namespace cff {

std::vector<CfbdPlayer> fetchPlayersFromCFBD(const std::string &baseUrl,
                                             const std::string &apiKey,
                                             const std::string &season,
                                             int maxTeams,
                                             std::vector<std::string> &errors,
                                             std::size_t &apiCalls,
                                             std::size_t &teamsExpected,
                                             std::size_t &teamsFetched) {
    const auto normalizedBase = trimTrailingSlash(
        baseUrl.empty() ? "https://api.collegefootballdata.com" : baseUrl
    );
    const int seasonYear = parseSeason(season, errors);
    if (seasonYear == 0) return {};

    // A full bulk refresh needs the team map and one classification-filtered
    // roster request. Keep one additional call in reserve so the job does not
    // intentionally consume the last available request in the monthly pool.
    constexpr long long kCallsRequiredAfterPreflight = 3;
    const auto quota = fetchQuota(normalizedBase, apiKey, errors, apiCalls);
    if (!quota) return {};
    if (*quota->remainingCalls < kCallsRequiredAfterPreflight) {
        std::string message = "CFBD quota preflight found " +
                              std::to_string(*quota->remainingCalls) +
                              " calls remaining; at least " +
                              std::to_string(kCallsRequiredAfterPreflight) +
                              " are required for a safe FBS roster refresh";
        if (!quota->resetAt.empty()) message += ". Quota resets at " + quota->resetAt;
        message += ". No roster request was made and the existing player catalog was preserved.";
        errors.push_back(std::move(message));
        return {};
    }

    const auto teams = fetchFbsTeams(normalizedBase, apiKey, season, errors, apiCalls);
    if (teams.empty()) {
        if (errors.empty()) errors.push_back("CFBD returned no FBS teams for season " + season + ".");
        return {};
    }

    const auto fetchLimit = std::min<std::size_t>(
        teams.size(),
        static_cast<std::size_t>(std::max(1, maxTeams))
    );
    teamsExpected = fetchLimit;
    if (fetchLimit < teams.size()) {
        errors.push_back("CFBD_MAX_TEAMS limited the refresh to " + std::to_string(fetchLimit) +
                         " of " + std::to_string(teams.size()) + " FBS teams; stale players were not retired.");
    }

    std::unordered_map<std::string, std::string> conferenceByTeam;
    std::unordered_set<std::string> selectedTeams;
    conferenceByTeam.reserve(teams.size());
    selectedTeams.reserve(fetchLimit);
    for (std::size_t index = 0; index < teams.size(); ++index) {
        conferenceByTeam.emplace(teams[index].school, teams[index].conference);
        if (index < fetchLimit) selectedTeams.insert(teams[index].school);
    }

    const auto rosterResponse = requestJson(
        normalizedBase + "/roster",
        apiKey,
        cpr::Parameters{{"year", season}, {"classification", "fbs"}},
        "CFBD bulk FBS roster",
        errors,
        apiCalls
    );
    if (!rosterResponse.ok) return {};
    if (!rosterResponse.payload.is_array()) {
        errors.push_back("CFBD bulk FBS roster returned an unexpected response shape.");
        return {};
    }
    if (rosterResponse.payload.empty()) {
        errors.push_back("CFBD returned an empty bulk FBS roster for season " + season + ".");
        return {};
    }

    std::vector<CfbdPlayer> players;
    std::unordered_map<std::string, std::size_t> playerIndexes;
    std::unordered_set<std::string> rosterTeams;
    players.reserve(rosterResponse.payload.size());
    playerIndexes.reserve(rosterResponse.payload.size());
    rosterTeams.reserve(fetchLimit);

    for (const auto &entry : rosterResponse.payload) {
        const auto teamName = stringFromKeys(entry, {"team", "school"});
        if (teamName.empty() || selectedTeams.find(teamName) == selectedTeams.end()) continue;
        rosterTeams.insert(teamName);

        CfbdPlayer player;
        player.id = stringFromKeys(entry, {"id", "athleteId", "playerId"});
        if (player.id.empty()) {
            std::cerr << "[cfbd] Skipping a " << teamName
                      << " roster entry without a player id." << std::endl;
            continue;
        }
        player.firstName = stringFromKeys(entry, {"first_name", "firstName"});
        player.lastName = stringFromKeys(entry, {"last_name", "lastName"});
        player.fullName = stringFromKeys(entry, {"name", "full_name", "fullName"});
        if (player.fullName.empty()) {
            player.fullName = player.firstName;
            if (!player.fullName.empty() && !player.lastName.empty()) player.fullName += " ";
            player.fullName += player.lastName;
        }
        if (player.fullName.empty()) player.fullName = "Player " + player.id;
        player.position = stringFromKeys(entry, {"position"});
        player.team = teamName;
        const auto conference = conferenceByTeam.find(teamName);
        player.conference = conference == conferenceByTeam.end() ? "" : conference->second;
        player.year = stringFromKeys(entry, {"year", "class"});
        player.height = stringFromKeys(entry, {"height"});
        player.weight = intFromKeys(entry, {"weight"});
        player.season = seasonYear;
        player.raw = entry;
        player.raw["cffTeam"] = teamName;
        player.raw["cffConference"] = player.conference;
        player.raw["cffSeason"] = seasonYear;

        const auto existing = playerIndexes.find(player.id);
        if (existing == playerIndexes.end()) {
            playerIndexes.emplace(player.id, players.size());
            players.push_back(std::move(player));
        } else {
            players[existing->second] = std::move(player);
        }
    }

    teamsFetched = rosterTeams.size();
    if (players.empty()) {
        errors.push_back("CFBD bulk FBS roster contained no players for the selected teams.");
        return {};
    }
    if (teamsFetched != teamsExpected) {
        errors.push_back("CFBD bulk FBS roster covered " + std::to_string(teamsFetched) +
                         " of " + std::to_string(teamsExpected) +
                         " expected teams; stale players were not retired.");
    }

    return players;
}

IngestResult upsertPlayersToPostgres(const std::vector<CfbdPlayer> &players,
                                     const std::string &dbUrl,
                                     int season,
                                     bool completeImport,
                                     std::vector<std::string> &errors) {
    IngestResult result;
    result.complete = completeImport;
    if (players.empty()) {
        errors.push_back("No roster players were available to upsert.");
        result.complete = false;
        return result;
    }

    try {
        pqxx::connection connection{dbUrl};
        if (!connection.is_open()) {
            errors.push_back("Unable to connect to Postgres with DB_URL.");
            result.complete = false;
            return result;
        }
        ensurePlayersSchema(connection);

        pqxx::work transaction{connection};
        transaction.exec(
            "CREATE TEMP TABLE current_player_import_ids (id TEXT PRIMARY KEY) ON COMMIT DROP"
        );

        for (const auto &player : players) {
            transaction.exec_params(
                "INSERT INTO current_player_import_ids (id) VALUES ($1) ON CONFLICT DO NOTHING",
                player.id
            );

            const std::string weight = player.weight ? std::to_string(*player.weight) : "";
            const auto upsert = transaction.exec_params(
                R"SQL(
                    INSERT INTO players (
                        id, full_name, first_name, last_name, position, team, conference,
                        year, height, weight, season, active, last_seen_at, raw
                    )
                    VALUES (
                        $1, $2, NULLIF($3, ''), NULLIF($4, ''), NULLIF($5, ''),
                        NULLIF($6, ''), NULLIF($7, ''), NULLIF($8, ''), NULLIF($9, ''),
                        NULLIF($10, '')::INT, $11, TRUE, NOW(), $12::jsonb
                    )
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
                        season = EXCLUDED.season,
                        active = TRUE,
                        last_seen_at = NOW(),
                        raw = EXCLUDED.raw,
                        updated_at = NOW()
                    RETURNING (xmax = 0) AS inserted
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
                weight,
                season,
                player.raw.dump()
            );

            if (!upsert.empty() && upsert[0][0].as<bool>()) {
                ++result.ingested;
            } else {
                ++result.updated;
            }
        }

        if (completeImport) {
            const auto retired = transaction.exec_params(
                R"SQL(
                    UPDATE players AS player
                    SET active = FALSE, updated_at = NOW()
                    WHERE player.active = TRUE
                      AND (
                        player.season IS DISTINCT FROM $1
                        OR NOT EXISTS (
                          SELECT 1 FROM current_player_import_ids AS imported
                          WHERE imported.id = player.id
                        )
                      )
                    RETURNING player.id
                )SQL",
                season
            );
            result.retired = retired.size();
        }

        transaction.commit();
    } catch (const std::exception &error) {
        errors.push_back(std::string{"Postgres player refresh failed: "} + error.what());
        result.complete = false;
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

    const auto baseUrl = readEnv("CFBD_API_BASE_URL").value_or(
        readEnv("CFBD_BASE_URL").value_or("https://api.collegefootballdata.com")
    );
    const auto seasonText = readEnv("CFBD_SEASON").value_or(currentYearString());
    const int season = parseSeason(seasonText, overall.errors);
    if (season == 0) return overall;
    const int maxTeams = configuredMaxTeams(overall.errors);

    std::size_t apiCalls = 0;
    std::size_t teamsExpected = 0;
    std::size_t teamsFetched = 0;
    auto players = fetchPlayersFromCFBD(
        baseUrl,
        *apiKey,
        seasonText,
        maxTeams,
        overall.errors,
        apiCalls,
        teamsExpected,
        teamsFetched
    );

    overall.apiCalls = apiCalls;
    overall.teamsExpected = teamsExpected;
    overall.teamsFetched = teamsFetched;
    if (players.empty()) {
        if (overall.errors.empty()) {
            overall.errors.push_back("No roster players were returned; the existing player catalog was preserved.");
        }
        recordIngestionRun(*dbUrl, season, overall);
        return overall;
    }

    const bool completeImport =
        teamsExpected > 0 &&
        teamsFetched == teamsExpected &&
        !players.empty() &&
        overall.errors.empty();

    auto databaseResult = upsertPlayersToPostgres(
        players,
        *dbUrl,
        season,
        completeImport,
        overall.errors
    );

    overall.ingested = databaseResult.ingested;
    overall.updated = databaseResult.updated;
    overall.retired = databaseResult.retired;
    overall.complete = completeImport && databaseResult.complete && overall.errors.empty();

    if (!overall.complete && overall.errors.empty()) {
        overall.errors.push_back("The player refresh was incomplete; stale players were kept active.");
    }
    recordIngestionRun(*dbUrl, season, overall);
    return overall;
}

} // namespace cff
