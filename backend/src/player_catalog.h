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
    int season = 0;
    std::string updatedAt;

    Json::Value toJson() const;
};

// Searches the active current-season player catalog. An empty query returns a
// browsable player pool, while non-empty tokens match name, team, position,
// and conference. Optional filters are applied server-side.
std::vector<PlayerCard> searchPlayers(const std::string &query,
                                      const std::optional<std::string> &positionFilter,
                                      const std::optional<std::string> &conferenceFilter,
                                      const std::optional<std::string> &teamFilter,
                                      std::size_t limit = 25,
                                      std::size_t offset = 0);

// Public, non-sensitive summary used by the player browser to show roster
// coverage and sync freshness without exposing admin ingestion details.
Json::Value playerCatalogMeta();

} // namespace cff
