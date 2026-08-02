#include "player_catalog.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#ifdef CFF_HAS_POSTGRES
#include <postgresql/libpq-fe.h>
#endif

namespace {

std::string toLower(const std::string &input) {
    std::string lowered = input;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return lowered;
}

std::vector<std::string> tokenizeQuery(const std::string &query) {
    std::vector<std::string> tokens;
    std::string current;
    for (char ch : query) {
        if (std::isspace(static_cast<unsigned char>(ch))) {
            if (!current.empty()) {
                tokens.push_back(toLower(current));
                current.clear();
            }
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty()) tokens.push_back(toLower(current));
    return tokens;
}

std::size_t clampLimit(std::size_t limit) {
    constexpr std::size_t kMax = 100;
    constexpr std::size_t kDefault = 25;
    if (limit == 0) return kDefault;
    return std::min(limit, kMax);
}

std::size_t clampOffset(std::size_t offset) {
    constexpr std::size_t kMax = 5000;
    return std::min(offset, kMax);
}

#ifdef CFF_HAS_POSTGRES
struct PgConnDeleter {
    void operator()(PGconn *connection) const {
        if (connection) PQfinish(connection);
    }
};

struct PgResultDeleter {
    void operator()(PGresult *result) const {
        if (result) PQclear(result);
    }
};

using PgConnPtr = std::unique_ptr<PGconn, PgConnDeleter>;
using PgResultPtr = std::unique_ptr<PGresult, PgResultDeleter>;

PgConnPtr connectToDb() {
    const char *url = std::getenv("DB_URL");
    if (!url) {
        std::cerr << "[players] DB_URL is not set; player search unavailable." << std::endl;
        return nullptr;
    }
    auto connection = PgConnPtr{PQconnectdb(url)};
    if (PQstatus(connection.get()) != CONNECTION_OK) {
        std::cerr << "[players] Failed to connect to Postgres: "
                  << PQerrorMessage(connection.get()) << std::endl;
        return nullptr;
    }
    return connection;
}

std::string buildLikeToken(const std::string &token) {
    return "%" + token + "%";
}

std::vector<const char*> buildParamPointers(const std::vector<std::string> &params) {
    std::vector<const char*> pointers;
    pointers.reserve(params.size());
    for (const auto &param : params) pointers.push_back(param.c_str());
    return pointers;
}
#endif

} // namespace

namespace cff {

Json::Value PlayerCard::toJson() const {
    Json::Value json;
    json["id"] = id;
    json["name"] = name;
    json["team"] = team;
    json["position"] = position;
    json["conference"] = conference;
    json["class"] = classYear;
    json["season"] = season;
    json["updatedAt"] = updatedAt;
    return json;
}

std::vector<PlayerCard> searchPlayers(const std::string &query,
                                      const std::optional<std::string> &positionFilter,
                                      const std::optional<std::string> &conferenceFilter,
                                      const std::optional<std::string> &teamFilter,
                                      std::size_t limit,
                                      std::size_t offset) {
    const auto tokens = tokenizeQuery(query);
    std::vector<PlayerCard> results;

#ifdef CFF_HAS_POSTGRES
    auto connection = connectToDb();
    if (!connection) return results;

    std::string sql = R"SQL(
        SELECT
            COALESCE(player.id, '') AS id,
            COALESCE(player.full_name, '') AS name,
            COALESCE(player.team, '') AS team,
            COALESCE(player.position, '') AS position,
            COALESCE(player.conference, '') AS conference,
            COALESCE(player.year, '') AS class,
            COALESCE(player.season, 0) AS season,
            COALESCE(player.updated_at::text, '') AS updated_at
        FROM players AS player
    )SQL";

    std::vector<std::string> params;
    std::vector<std::string> whereClauses{"player.active = TRUE"};
    params.reserve(tokens.size() + 5);

    for (const auto &token : tokens) {
        params.push_back(buildLikeToken(token));
        const auto index = params.size();
        whereClauses.push_back(
            "(player.full_name ILIKE $" + std::to_string(index) +
            " OR player.team ILIKE $" + std::to_string(index) +
            " OR player.position ILIKE $" + std::to_string(index) +
            " OR player.conference ILIKE $" + std::to_string(index) + ")"
        );
    }

    if (positionFilter && !positionFilter->empty()) {
        params.push_back(*positionFilter);
        whereClauses.push_back("player.position ILIKE $" + std::to_string(params.size()));
    }

    if (conferenceFilter && !conferenceFilter->empty()) {
        params.push_back(*conferenceFilter);
        whereClauses.push_back("player.conference ILIKE $" + std::to_string(params.size()));
    }

    if (teamFilter && !teamFilter->empty()) {
        params.push_back(*teamFilter);
        whereClauses.push_back("player.team ILIKE $" + std::to_string(params.size()));
    }

    sql += " WHERE ";
    for (std::size_t index = 0; index < whereClauses.size(); ++index) {
        if (index > 0) sql += " AND ";
        sql += whereClauses[index];
    }

    sql += R"SQL(
        ORDER BY
            player.season DESC NULLS LAST,
            CASE UPPER(COALESCE(player.position, ''))
                WHEN 'QB' THEN 1
                WHEN 'RB' THEN 2
                WHEN 'WR' THEN 3
                WHEN 'TE' THEN 4
                WHEN 'K' THEN 5
                ELSE 6
            END,
            player.full_name ASC
        LIMIT $
    )SQL";
    sql += std::to_string(params.size() + 1);
    sql += " OFFSET $" + std::to_string(params.size() + 2);

    params.push_back(std::to_string(clampLimit(limit)));
    params.push_back(std::to_string(clampOffset(offset)));
    const auto paramPointers = buildParamPointers(params);
    PgResultPtr result{PQexecParams(
        connection.get(),
        sql.c_str(),
        static_cast<int>(params.size()),
        nullptr,
        paramPointers.data(),
        nullptr,
        nullptr,
        0
    )};

    if (PQresultStatus(result.get()) != PGRES_TUPLES_OK) {
        std::cerr << "[players] Query failed: " << PQerrorMessage(connection.get()) << std::endl;
        return results;
    }

    const auto rows = PQntuples(result.get());
    results.reserve(static_cast<std::size_t>(rows));
    for (int row = 0; row < rows; ++row) {
        PlayerCard player;
        player.id = PQgetvalue(result.get(), row, 0);
        player.name = PQgetvalue(result.get(), row, 1);
        player.team = PQgetvalue(result.get(), row, 2);
        player.position = PQgetvalue(result.get(), row, 3);
        player.conference = PQgetvalue(result.get(), row, 4);
        player.classYear = PQgetvalue(result.get(), row, 5);
        player.season = std::atoi(PQgetvalue(result.get(), row, 6));
        player.updatedAt = PQgetvalue(result.get(), row, 7);
        results.push_back(std::move(player));
    }
#else
    (void)query;
    (void)positionFilter;
    (void)conferenceFilter;
    (void)teamFilter;
    (void)limit;
    (void)offset;
    std::cerr << "[players] Built without PostgreSQL; player search is disabled." << std::endl;
#endif

    return results;
}

Json::Value playerCatalogMeta() {
    Json::Value payload;
#ifdef CFF_HAS_POSTGRES
    auto connection = connectToDb();
    payload["databaseConfigured"] = static_cast<bool>(connection);
    if (!connection) {
        payload["status"] = "unavailable";
        return payload;
    }

    PgResultPtr summary{PQexec(connection.get(), R"SQL(
        SELECT COUNT(*), COALESCE(MAX(season), 0),
               COALESCE(MAX(updated_at)::text, ''),
               COUNT(DISTINCT NULLIF(team, '')),
               COUNT(DISTINCT NULLIF(conference, ''))
        FROM players WHERE active = TRUE
    )SQL")};
    if (PQresultStatus(summary.get()) != PGRES_TUPLES_OK || PQntuples(summary.get()) == 0) {
        payload["status"] = "unavailable";
        return payload;
    }
    payload["status"] = "ok";
    payload["activePlayers"] = static_cast<Json::Int64>(std::atoll(PQgetvalue(summary.get(), 0, 0)));
    payload["season"] = std::atoi(PQgetvalue(summary.get(), 0, 1));
    payload["lastUpdated"] = PQgetvalue(summary.get(), 0, 2);
    payload["teams"] = static_cast<Json::Int64>(std::atoll(PQgetvalue(summary.get(), 0, 3)));
    payload["conferences"] = static_cast<Json::Int64>(std::atoll(PQgetvalue(summary.get(), 0, 4)));

    Json::Value positions(Json::objectValue);
    PgResultPtr positionRows{PQexec(connection.get(), R"SQL(
        SELECT UPPER(COALESCE(NULLIF(position, ''), 'OTHER')), COUNT(*)
        FROM players WHERE active = TRUE
        GROUP BY 1 ORDER BY 2 DESC, 1 ASC
    )SQL")};
    if (PQresultStatus(positionRows.get()) == PGRES_TUPLES_OK) {
        for (int row = 0; row < PQntuples(positionRows.get()); ++row) {
            positions[PQgetvalue(positionRows.get(), row, 0)] =
                static_cast<Json::Int64>(std::atoll(PQgetvalue(positionRows.get(), row, 1)));
        }
    }
    payload["positions"] = positions;
#else
    payload["databaseConfigured"] = false;
    payload["status"] = "unavailable";
#endif
    return payload;
}

} // namespace cff
