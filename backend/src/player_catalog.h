#pragma once

#include <json/json.h>
#include <optional>
#include <string>
#include <vector>

namespace cff {

struct PlayerCard {
    std::string id;
    std::string name;
    std::string team;
    std::string position;
    std::string conference;
    std::string classYear;

    Json::Value toJson() const;
};

// Searches the players table (joined with teams) using the Postgres schema in db/schema.sql.
// - query: free-text tokens matched against name, team, position, and conference.
// - positionFilter: exact position match (e.g., QB, RB, WR).
// - conferenceFilter: exact conference match (e.g., SEC, Big Ten).
// - limit: maximum number of rows returned (clamped internally).
std::vector<PlayerCard> searchPlayers(const std::string &query,
                                      const std::optional<std::string> &positionFilter,
                                      const std::optional<std::string> &conferenceFilter,
                                      std::size_t limit = 25);

} // namespace cff
