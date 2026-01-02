#pragma once

#include <string>
#include <vector>
#include <optional>
#include <nlohmann/json.hpp>

namespace cff {

struct CfbdPlayer {
    std::string id;
    std::string fullName;
    std::string firstName;
    std::string lastName;
    std::string position;
    std::string team;
    std::string conference;
    std::string year;
    std::string height;
    std::optional<int> weight;
    nlohmann::json raw;
};

struct IngestResult {
    std::size_t ingested = 0;
    std::size_t updated = 0;
    std::vector<std::string> errors;
};

// Fetches players from CFBD using the provided base URL, API key, and season.
std::vector<CfbdPlayer> fetchPlayersFromCFBD(const std::string &baseUrl,
                                             const std::string &apiKey,
                                             const std::string &season,
                                             std::vector<std::string> &errors);

// Upserts the provided players into Postgres using DB_URL. Ensures the table
// and indexes exist before inserting.
IngestResult upsertPlayersToPostgres(const std::vector<CfbdPlayer> &players,
                                     const std::string &dbUrl,
                                     std::vector<std::string> &errors);

// Runs a single-shot ingest using environment variables for configuration.
// - CFBD_API_KEY (required)
// - CFBD_BASE_URL (optional; defaults to https://api.collegefootballdata.com)
// - CFBD_SEASON (optional; defaults to current year)
// - DB_URL (required)
IngestResult runCfbdIngestOnce();

} // namespace cff
