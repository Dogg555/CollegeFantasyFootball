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
    if (!current.empty()) {
        tokens.push_back(toLower(current));
    }
    return tokens;
}

std::size_t clampLimit(std::size_t limit) {
    constexpr std::size_t kMax = 100;
    constexpr std::size_t kDefault = 25;
    if (limit == 0) {
        return kDefault;
    }
    return std::min(limit, kMax);
}

#ifdef CFF_HAS_POSTGRES
struct PgConnDeleter {
    void operator()(PGconn* conn) const {
        if (conn) {
            PQfinish(conn);
        }
    }
};

struct PgResultDeleter {
    void operator()(PGresult* res) const {
        if (res) {
            PQclear(res);
        }
    }
};

using PgConnPtr = std::unique_ptr<PGconn, PgConnDeleter>;
using PgResultPtr = std::unique_ptr<PGresult, PgResultDeleter>;

PgConnPtr connectToDb() {
    const char* url = std::getenv("DB_URL");
    if (!url) {
        std::cerr << "[players] DB_URL is not set; player search unavailable." << std::endl;
        return nullptr;
    }
    auto conn = PgConnPtr{PQconnectdb(url)};
    if (PQstatus(conn.get()) != CONNECTION_OK) {
        std::cerr << "[players] Failed to connect to Postgres: " << PQerrorMessage(conn.get()) << std::endl;
        return nullptr;
    }
    return conn;
}

std::string buildLikeToken(const std::string &token) {
    return "%" + token + "%";
}

std::vector<const char*> buildParamPointers(const std::vector<std::string> &params) {
    std::vector<const char*> ptrs;
    ptrs.reserve(params.size());
    for (const auto &p : params) {
        ptrs.push_back(p.c_str());
    }
    return ptrs;
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
    return json;
}

std::vector<PlayerCard> searchPlayers(const std::string &query,
                                      const std::optional<std::string> &positionFilter,
                                      const std::optional<std::string> &conferenceFilter,
                                      std::size_t limit) {
    const auto tokens = tokenizeQuery(query);
    if (tokens.empty()) {
        return {};
    }

    std::vector<PlayerCard> results;

#ifdef CFF_HAS_POSTGRES
    auto conn = connectToDb();
    if (!conn) {
        return results;
    }

    std::string sql = R"SQL(
        SELECT
            COALESCE(p.provider_player_id, 'player-' || p.id::text) AS id,
            COALESCE(p.full_name, '') AS name,
            COALESCE(t.abbreviation, COALESCE(t.school, '')) AS team,
            COALESCE(p.position, '') AS position,
            COALESCE(t.conference, '') AS conference,
            COALESCE(p.class, '') AS class
        FROM players p
        LEFT JOIN teams t ON p.team_id = t.id
    )SQL";

    std::vector<std::string> params;
    std::vector<std::string> whereClauses;
    params.reserve(tokens.size() + 2); // filters and tokens

    for (const auto &token : tokens) {
        params.push_back(buildLikeToken(token));
        const auto idx = params.size();
        whereClauses.push_back(
            "(p.full_name ILIKE $" + std::to_string(idx) +
            " OR t.school ILIKE $" + std::to_string(idx) +
            " OR t.abbreviation ILIKE $" + std::to_string(idx) +
            " OR p.position ILIKE $" + std::to_string(idx) +
            " OR t.conference ILIKE $" + std::to_string(idx) + ")"
        );
    }

    if (positionFilter && !positionFilter->empty()) {
        params.push_back(*positionFilter);
        const auto idx = params.size();
        whereClauses.push_back("p.position ILIKE $" + std::to_string(idx));
    }

    if (conferenceFilter && !conferenceFilter->empty()) {
        params.push_back(*conferenceFilter);
        const auto idx = params.size();
        whereClauses.push_back("t.conference ILIKE $" + std::to_string(idx));
    }

    if (!whereClauses.empty()) {
        sql += " WHERE ";
        for (std::size_t i = 0; i < whereClauses.size(); ++i) {
            sql += whereClauses[i];
            if (i + 1 < whereClauses.size()) {
                sql += " AND ";
            }
        }
    }

    sql += " ORDER BY p.full_name ASC LIMIT $" + std::to_string(params.size() + 1);

    const auto clamped = clampLimit(limit);
    params.push_back(std::to_string(clamped));

    const auto paramPtrs = buildParamPointers(params);
    PgResultPtr res{PQexecParams(conn.get(),
                                 sql.c_str(),
                                 static_cast<int>(params.size()),
                                 nullptr,
                                 paramPtrs.data(),
                                 nullptr,
                                 nullptr,
                                 0)};

    if (PQresultStatus(res.get()) != PGRES_TUPLES_OK) {
        std::cerr << "[players] Query failed: " << PQerrorMessage(conn.get()) << std::endl;
        return results;
    }

    const auto rows = PQntuples(res.get());
    results.reserve(static_cast<std::size_t>(rows));

    for (int i = 0; i < rows; ++i) {
        PlayerCard player;
        player.id = PQgetvalue(res.get(), i, 0);
        player.name = PQgetvalue(res.get(), i, 1);
        player.team = PQgetvalue(res.get(), i, 2);
        player.position = PQgetvalue(res.get(), i, 3);
        player.conference = PQgetvalue(res.get(), i, 4);
        player.classYear = PQgetvalue(res.get(), i, 5);
        results.push_back(std::move(player));
    }
#else
    (void)positionFilter;
    (void)conferenceFilter;
    (void)limit;
    std::cerr << "[players] Built without PostgreSQL; player search is disabled." << std::endl;
#endif

    return results;
}

} // namespace cff
