#pragma once

#include <cstddef>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

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
    int season = 0;
    nlohmann::json raw;
};

struct IngestResult {
    std::size_t ingested = 0;
    std::size_t updated = 0;
    std::size_t retired = 0;
    std::size_t apiCalls = 0;
    std::size_t teamsExpected = 0;
    std::size_t teamsFetched = 0;
    bool complete = false;
    std::vector<std::string> errors;
};

// Fetch the current FBS team list and then each team's season roster. The
// maxTeams guardrail protects the monthly CFBD allowance. teamsExpected and
// teamsFetched let callers determine whether it is safe to retire stale rows.
std::vector<CfbdPlayer> fetchPlayersFromCFBD(const std::string &baseUrl,
                                             const std::string &apiKey,
                                             const std::string &season,
                                             int maxTeams,
                                             std::vector<std::string> &errors,
                                             std::size_t &apiCalls,
                                             std::size_t &teamsExpected,
                                             std::size_t &teamsFetched);

// Upsert players into Postgres. Missing players are marked inactive only when
// the caller confirms that every expected FBS roster was fetched successfully.
IngestResult upsertPlayersToPostgres(const std::vector<CfbdPlayer> &players,
                                     const std::string &dbUrl,
                                     int season,
                                     bool completeImport,
                                     std::vector<std::string> &errors);

// Runs one season-aware roster refresh using:
// - CFBD_API_KEY (required)
// - CFBD_API_BASE_URL or CFBD_BASE_URL (optional)
// - CFBD_SEASON (optional; defaults to current UTC year)
// - CFBD_MAX_TEAMS (optional; defaults to 200)
// - DB_URL (required)
IngestResult runCfbdIngestOnce();

} // namespace cff
