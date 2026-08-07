std::optional<Json::Value> authoritativePlayerSnapshot(PGconn *connection,
                                                       const std::string &playerId) {
    if (!connection || playerId.empty()) return std::nullopt;
    auto result = execute(connection,
        "SELECT id, full_name, COALESCE(team, ''), COALESCE(position, ''), "
        "COALESCE(conference, ''), COALESCE(year, ''), COALESCE(season, 0), "
        "COALESCE(to_char(updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), '') "
        "FROM players WHERE id = $1 AND active = TRUE LIMIT 1 FOR SHARE",
        {playerId});
    if (!tuplesOk(result) || PQntuples(result.get()) == 0) return std::nullopt;

    Json::Value player(Json::objectValue);
    player["id"] = cell(result.get(), 0, 0);
    player["playerId"] = cell(result.get(), 0, 0);
    player["name"] = cell(result.get(), 0, 1);
    player["team"] = cell(result.get(), 0, 2);
    player["school"] = cell(result.get(), 0, 2);
    player["position"] = cell(result.get(), 0, 3);
    player["conference"] = cell(result.get(), 0, 4);
    player["class"] = cell(result.get(), 0, 5);
    player["season"] = static_cast<Json::Int64>(cellInt64(result.get(), 0, 6, 0));
    player["updatedAt"] = cell(result.get(), 0, 7);
    return player;
}

bool freeAgentPlayerPoolEligible(const Json::Value &player,
                                 const Json::Value &rosterRules) {
    const auto position = lower(trim(jsonStringMember(player, "position")));
    if (position == "rb" || position == "wr" || position == "te") {
        return cff::league_roster::slotLimit(rosterRules, position) > 0
            || cff::league_roster::slotLimit(rosterRules, "flex") > 0;
    }
    if (position == "qb" || position == "k") {
        return cff::league_roster::slotLimit(rosterRules, position) > 0;
    }
    if (position == "def" || position == "dst") {
        return cff::league_roster::slotLimit(rosterRules, "def") > 0;
    }
    return false;
}

std::string rosterServerTime(PGconn *connection) {
    auto result = execute(connection,
        "SELECT COALESCE(to_char(NOW() AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), '')");
    return tuplesOk(result) && PQntuples(result.get()) > 0 ? cell(result.get(), 0, 0) : "";
}

bool freeAgentPlayerLocked(PGconn *connection,
                           const std::string &leagueId,
                           const std::string &team) {
    if (!connection || leagueId.empty() || trim(team).empty()) return false;
    auto result = execute(connection,
        "WITH active_week AS ("
        "SELECT week_state.season, week_state.week FROM schedule_week_states week_state "
        "LEFT JOIN scoring_week_states scoring ON scoring.league_id = week_state.league_id "
        "AND scoring.season = week_state.season AND scoring.week = week_state.week "
        "WHERE week_state.league_id = $1 AND COALESCE(scoring.status, 'unscored') <> 'final' "
        "ORDER BY week_state.season DESC, week_state.week ASC LIMIT 1) "
        "SELECT COALESCE(EXISTS (SELECT 1 FROM active_week week_context JOIN games game "
        "ON game.season = week_context.season AND game.week = week_context.week "
        "WHERE (lower(game.home_team) = lower($2) OR lower(game.away_team) = lower($2)) "
        "AND game.start_date IS NOT NULL AND game.start_date <= NOW()), FALSE)",
        {leagueId, team});
    return tuplesOk(result) && PQntuples(result.get()) > 0 && cell(result.get(), 0, 0) == "t";
}

std::size_t freeAgentDirectorySizeParam(const drogon::HttpRequestPtr &request,
                                        const std::string &key,
                                        std::size_t fallback,
                                        std::size_t maximum) {
    const auto raw = request->getParameter(key);
    if (raw.empty()) return fallback;
    char *end = nullptr;
    const auto parsed = std::strtoul(raw.c_str(), &end, 10);
    if (end == raw.c_str() || *end != '\0') return fallback;
    return std::min<std::size_t>(parsed, maximum);
}

Json::Value freeAgentDirectoryCapabilities() {
    Json::Value capabilities(Json::objectValue);
    capabilities["availability"] = true;
    capabilities["opponent"] = true;
    capabilities["gameStatus"] = true;
    capabilities["rosterPercentage"] = true;
    capabilities["points"] = false;
    capabilities["projections"] = false;
    capabilities["pointsNote"] = "League-scored free-agent points are not stored for unrostered players yet.";
    capabilities["projectionsNote"] = "Projection data is not currently ingested.";
    return capabilities;
}

drogon::HttpResponsePtr getFreeAgentDirectory(const drogon::HttpRequestPtr &request,
                                              const std::string &leagueId,
                                              const std::string &email) {
    auto connection = connectDb();
    if (!connection || !begin(connection.get())) {
        return errorResponse(drogon::k503ServiceUnavailable,
                             "Player availability is temporarily unavailable.",
                             "player_directory_unavailable",
                             true);
    }

    const auto access = leagueAccess(connection.get(), leagueId, email);
    if (!access.exists) {
        rollback(connection.get());
        return errorResponse(drogon::k404NotFound, "League not found.", "league_not_found");
    }
    if (!access.member) {
        rollback(connection.get());
        return errorResponse(drogon::k403Forbidden,
                             "Active league membership is required.",
                             "league_membership_required");
    }
    if (!ensureRosterState(connection.get(), leagueId, email)) {
        rollback(connection.get());
        return errorResponse(drogon::k503ServiceUnavailable,
                             "Roster state could not be initialized.",
                             "roster_state_unavailable",
                             true);
    }

    const auto directAllowed = cff::roster_transaction::directAcquisitionAllowed(access.waiverRules);
    const auto query = trim(request->getParameter("query"));
    const auto position = trim(request->getParameter("position"));
    const auto conference = trim(request->getParameter("conference"));
    const auto team = trim(request->getParameter("team"));
    const auto availability = lower(trim(request->getParameter("availability")));
    const auto opponent = trim(request->getParameter("opponent"));
    const auto gameStatus = lower(trim(request->getParameter("gameStatus")));
    const auto limit = freeAgentDirectorySizeParam(request, "limit", 50, 100);
    const auto offset = freeAgentDirectorySizeParam(request, "offset", 0, 5000);

    std::vector<std::string> parameters{
        leagueId,
        email,
        directAllowed ? "true" : "false",
        jsonToString(access.rosterRules)
    };
    std::vector<std::string> predicates;
    auto addLikePredicate = [&](const std::string &value, const std::string &expression) {
        if (value.empty()) return;
        parameters.push_back("%" + value + "%");
        predicates.push_back(expression + " ILIKE $" + std::to_string(parameters.size()));
    };
    addLikePredicate(query,
        "(directory.name || ' ' || directory.team || ' ' || directory.position || ' ' || directory.conference)");
    addLikePredicate(position, "directory.position");
    addLikePredicate(conference, "directory.conference");
    addLikePredicate(team, "directory.team");
    addLikePredicate(opponent, "directory.opponent");
    if (!availability.empty()) {
        parameters.push_back(availability);
        predicates.push_back("directory.availability = $" + std::to_string(parameters.size()));
    }
    if (!gameStatus.empty()) {
        parameters.push_back(gameStatus);
        predicates.push_back("directory.game_status = $" + std::to_string(parameters.size()));
    }

    std::string sql = R"SQL(
        WITH active_week AS (
          SELECT week_state.season, week_state.week
          FROM schedule_week_states week_state
          LEFT JOIN scoring_week_states scoring
            ON scoring.league_id = week_state.league_id
           AND scoring.season = week_state.season
           AND scoring.week = week_state.week
          WHERE week_state.league_id = $1
            AND COALESCE(scoring.status, 'unscored') <> 'final'
          ORDER BY week_state.season DESC, week_state.week ASC
          LIMIT 1
        ), roster_rules AS (
          SELECT
            COALESCE(($4::jsonb ->> 'qb')::int, 0) AS qb,
            COALESCE(($4::jsonb ->> 'rb')::int, 0) AS rb,
            COALESCE(($4::jsonb ->> 'wr')::int, 0) AS wr,
            COALESCE(($4::jsonb ->> 'te')::int, 0) AS te,
            COALESCE(($4::jsonb ->> 'flex')::int, 0) AS flex,
            COALESCE(($4::jsonb ->> 'k')::int, 0) AS k,
            COALESCE(($4::jsonb ->> 'def')::int, 0) AS def
        ), directory AS (
          SELECT
            player.id,
            player.full_name AS name,
            COALESCE(player.team, '') AS team,
            COALESCE(player.position, '') AS position,
            COALESCE(player.conference, '') AS conference,
            COALESCE(player.year, '') AS class,
            COALESCE(player.season, 0) AS season,
            COALESCE(to_char(player.updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"'), '') AS updated_at,
            COALESCE(owner.owner_email, '') AS owner_email,
            COALESCE(owner.acquired_via, '') AS acquired_via,
            COALESCE(waiver.pending, FALSE) AS pending_waiver,
            COALESCE(game.game_id, '') AS game_id,
            COALESCE(game.opponent, '') AS opponent,
            COALESCE(game.start_time, '') AS game_start_time,
            COALESCE(game.locked, FALSE) AS game_locked,
            CASE
              WHEN COALESCE(game.game_id, '') = '' THEN 'bye'
              WHEN COALESCE(game.locked, FALSE) THEN 'started'
              ELSE 'scheduled'
            END AS game_status,
            COALESCE((
              SELECT ROUND(100.0 * COUNT(DISTINCT roster_count.league_id)
                / NULLIF((SELECT COUNT(*) FROM leagues), 0), 1)
              FROM rosters roster_count
              WHERE roster_count.player_id = player.id
            ), 0) AS roster_percentage,
            CASE
              WHEN NOT (
                (lower(COALESCE(player.position, '')) = 'qb' AND rules.qb > 0)
                OR (lower(COALESCE(player.position, '')) = 'rb' AND (rules.rb > 0 OR rules.flex > 0))
                OR (lower(COALESCE(player.position, '')) = 'wr' AND (rules.wr > 0 OR rules.flex > 0))
                OR (lower(COALESCE(player.position, '')) = 'te' AND (rules.te > 0 OR rules.flex > 0))
                OR (lower(COALESCE(player.position, '')) = 'k' AND rules.k > 0)
                OR (lower(COALESCE(player.position, '')) IN ('def', 'dst') AND rules.def > 0)
              ) THEN 'ineligible'
              WHEN COALESCE(owner.owner_email, '') <> '' AND owner.owner_email = lower($2) THEN 'rostered'
              WHEN COALESCE(owner.owner_email, '') <> '' THEN 'owned'
              WHEN COALESCE(game.locked, FALSE) THEN 'locked'
              WHEN COALESCE(waiver.pending, FALSE) OR NOT $3::boolean THEN 'waivers'
              ELSE 'available'
            END AS availability
          FROM players player
          CROSS JOIN roster_rules rules
          LEFT JOIN LATERAL (
            SELECT lower(roster.manager_email) AS owner_email, roster.acquired_via
            FROM rosters roster
            WHERE roster.league_id = $1 AND roster.player_id = player.id
            LIMIT 1
          ) owner ON TRUE
          LEFT JOIN LATERAL (
            SELECT TRUE AS pending
            FROM waiver_claims claim
            WHERE claim.league_id = $1 AND claim.add_player_id = player.id AND claim.status = 'pending'
            LIMIT 1
          ) waiver ON TRUE
          LEFT JOIN LATERAL (
            SELECT
              scheduled.id::text AS game_id,
              CASE
                WHEN lower(scheduled.home_team) = lower(COALESCE(player.team, '')) THEN COALESCE(scheduled.away_team, '')
                ELSE COALESCE(scheduled.home_team, '')
              END AS opponent,
              COALESCE(to_char(scheduled.start_date AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS"Z"'), '') AS start_time,
              COALESCE(scheduled.start_date <= NOW(), FALSE) AS locked
            FROM active_week week_context
            JOIN games scheduled
              ON scheduled.season = week_context.season
             AND scheduled.week = week_context.week
            WHERE COALESCE(player.team, '') <> ''
              AND (lower(scheduled.home_team) = lower(player.team)
                OR lower(scheduled.away_team) = lower(player.team))
            ORDER BY scheduled.start_date NULLS LAST, scheduled.id
            LIMIT 1
          ) game ON TRUE
          WHERE player.active = TRUE
        )
        SELECT id, name, team, position, conference, class, season, updated_at,
               owner_email, acquired_via, pending_waiver, game_id, opponent,
               game_start_time, game_locked, game_status, roster_percentage, availability
        FROM directory
    )SQL";

    if (!predicates.empty()) {
        sql += " WHERE ";
        for (std::size_t index = 0; index < predicates.size(); ++index) {
            if (index > 0) sql += " AND ";
            sql += predicates[index];
        }
    }
    sql += " ORDER BY CASE availability WHEN 'available' THEN 0 WHEN 'waivers' THEN 1 "
           "WHEN 'rostered' THEN 2 WHEN 'owned' THEN 3 ELSE 4 END, "
           "roster_percentage DESC, name ASC";
    parameters.push_back(std::to_string(limit));
    sql += " LIMIT $" + std::to_string(parameters.size()) + "::int";
    parameters.push_back(std::to_string(offset));
    sql += " OFFSET $" + std::to_string(parameters.size()) + "::int";

    auto result = execute(connection.get(), sql, parameters);
    if (!tuplesOk(result)) {
        rollback(connection.get());
        return errorResponse(drogon::k503ServiceUnavailable,
                             "Player availability could not be loaded.",
                             "player_directory_unavailable",
                             true);
    }

    Json::Value items(Json::arrayValue);
    for (int row = 0; row < PQntuples(result.get()); ++row) {
        Json::Value player(Json::objectValue);
        player["id"] = cell(result.get(), row, 0);
        player["playerId"] = cell(result.get(), row, 0);
        player["name"] = cell(result.get(), row, 1);
        player["team"] = cell(result.get(), row, 2);
        player["school"] = cell(result.get(), row, 2);
        player["position"] = cell(result.get(), row, 3);
        player["conference"] = cell(result.get(), row, 4);
        player["class"] = cell(result.get(), row, 5);
        player["season"] = static_cast<Json::Int64>(cellInt64(result.get(), row, 6, 0));
        player["updatedAt"] = cell(result.get(), row, 7);
        player["ownerEmail"] = cell(result.get(), row, 8);
        player["ownedByCurrentUser"] = cell(result.get(), row, 8) == email;
        player["acquiredVia"] = cell(result.get(), row, 9);
        player["pendingWaiver"] = cell(result.get(), row, 10) == "t";
        player["gameId"] = cell(result.get(), row, 11);
        player["opponent"] = cell(result.get(), row, 12);
        player["gameStartTime"] = cell(result.get(), row, 13);
        player["locked"] = cell(result.get(), row, 14) == "t";
        player["gameStatus"] = cell(result.get(), row, 15);
        try {
            player["rosterPercentage"] = std::stod(cell(result.get(), row, 16));
        } catch (...) {
            player["rosterPercentage"] = 0.0;
        }
        player["availability"] = cell(result.get(), row, 17);
        player["points"] = Json::Value{};
        player["projection"] = Json::Value{};
        items.append(player);
    }

    Json::Value payload(Json::objectValue);
    payload["leagueId"] = leagueId;
    payload["managerEmail"] = email;
    payload["serverTime"] = rosterServerTime(connection.get());
    payload["rosterVersion"] = Json::Int64(rosterVersion(connection.get(), leagueId, email));
    payload["waiverMode"] = access.waiverRules.get("mode", "free_agency").asString();
    payload["directAcquisitionAllowed"] = directAllowed;
    payload["items"] = items;
    payload["count"] = static_cast<Json::UInt64>(items.size());
    payload["limit"] = static_cast<Json::UInt64>(limit);
    payload["offset"] = static_cast<Json::UInt64>(offset);
    payload["capabilities"] = freeAgentDirectoryCapabilities();
    Json::Value states(Json::arrayValue);
    for (const auto *state : {"available", "rostered", "owned", "waivers", "locked", "ineligible",
                              "injured_reserve", "suspended", "drafted", "released"}) {
        states.append(state);
    }
    payload["availabilityStates"] = states;

    if (!commit(connection.get())) {
        rollback(connection.get());
        return errorResponse(drogon::k503ServiceUnavailable,
                             "Player availability could not be confirmed.",
                             "player_directory_unavailable",
                             true);
    }
    return jsonResponse(payload);
}
