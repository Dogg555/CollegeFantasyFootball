struct PgConnectionDeleter {
    void operator()(PGconn *connection) const {
        if (connection) PQfinish(connection);
    }
};

struct PgResultDeleter {
    void operator()(PGresult *result) const {
        if (result) PQclear(result);
    }
};

using PgConnection = std::unique_ptr<PGconn, PgConnectionDeleter>;
using PgResult = std::unique_ptr<PGresult, PgResultDeleter>;

bool dbConfigured() {
    const auto *value = std::getenv("DB_URL");
    return value && *value;
}

PgConnection connectDb() {
    const auto *value = std::getenv("DB_URL");
    if (!value) return nullptr;
    PgConnection connection{PQconnectdb(value)};
    if (!connection || PQstatus(connection.get()) != CONNECTION_OK) return nullptr;
    return connection;
}

PgResult execute(PGconn *connection,
                 const std::string &sql,
                 const std::vector<std::string> &parameters = {}) {
    std::vector<const char *> values;
    values.reserve(parameters.size());
    for (const auto &parameter : parameters) values.push_back(parameter.c_str());
    return PgResult{PQexecParams(connection,
                                 sql.c_str(),
                                 static_cast<int>(values.size()),
                                 nullptr,
                                 values.empty() ? nullptr : values.data(),
                                 nullptr,
                                 nullptr,
                                 0)};
}

bool tuplesOk(const PgResult &result) {
    return result && PQresultStatus(result.get()) == PGRES_TUPLES_OK;
}

bool commandOk(const PgResult &result) {
    return result && PQresultStatus(result.get()) == PGRES_COMMAND_OK;
}

std::string cell(PGresult *result, int row, int column) {
    return PQgetisnull(result, row, column) ? "" : PQgetvalue(result, row, column);
}

long long cellInt64(PGresult *result, int row, int column, long long fallback = 0) {
    const auto value = cell(result, row, column);
    if (value.empty()) return fallback;
    try {
        return std::stoll(value);
    } catch (...) {
        return fallback;
    }
}

double cellDouble(PGresult *result, int row, int column, double fallback = 0.0) {
    const auto value = cell(result, row, column);
    if (value.empty()) return fallback;
    try {
        return std::stod(value);
    } catch (...) {
        return fallback;
    }
}

bool begin(PGconn *connection) {
    return commandOk(execute(connection, "BEGIN"));
}

bool commit(PGconn *connection) {
    return commandOk(execute(connection, "COMMIT"));
}

void rollback(PGconn *connection) {
    (void)execute(connection, "ROLLBACK");
}

bool lockScoringLeague(PGconn *connection, const std::string &leagueId) {
    return tuplesOk(execute(connection,
        "SELECT pg_advisory_xact_lock(hashtextextended($1, 0))",
        {"scoring:" + leagueId}));
}

struct ScoringAccess {
    bool exists{false};
    bool member{false};
    bool commissioner{false};
    Json::Value scoringSettings{Json::objectValue};
    Json::Value rosterRules{Json::objectValue};
};

ScoringAccess scoringAccess(PGconn *connection,
                            const std::string &leagueId,
                            const std::string &email) {
    ScoringAccess access;
    auto result = execute(connection,
        "SELECT lower(l.account_email), l.scoring_settings::text, l.roster_rules::text, "
        "COALESCE(lower(lm.email), ''), COALESCE(lm.role, ''), COALESCE(lm.status, '') "
        "FROM leagues l LEFT JOIN league_members lm "
        "ON lm.league_id = l.id AND lower(lm.email) = lower($2) "
        "WHERE l.id = $1 LIMIT 1",
        {leagueId, email});
    if (!tuplesOk(result) || PQntuples(result.get()) == 0) return access;
    access.exists = true;
    const auto owner = canonicalEmail(cell(result.get(), 0, 0));
    const auto memberEmail = canonicalEmail(cell(result.get(), 0, 3));
    const auto role = lower(cell(result.get(), 0, 4));
    const auto status = lower(cell(result.get(), 0, 5));
    access.member = owner == email || (memberEmail == email && status == "active");
    access.commissioner = owner == email || (memberEmail == email && role == "commissioner" && status == "active");
    access.scoringSettings = jsonFromString(cell(result.get(), 0, 1));
    access.rosterRules = jsonFromString(cell(result.get(), 0, 2));
    return access;
}

bool ensureScoringState(PGconn *connection, const std::string &leagueId) {
    return commandOk(execute(connection,
        "INSERT INTO scoring_states (league_id, version, standings_version, updated_at) "
        "VALUES ($1, 0, 0, NOW()) ON CONFLICT (league_id) DO NOTHING",
        {leagueId}));
}

std::pair<long long, long long> scoringVersions(PGconn *connection,
                                                const std::string &leagueId) {
    auto result = execute(connection,
        "SELECT version, standings_version FROM scoring_states WHERE league_id = $1 LIMIT 1",
        {leagueId});
    if (!tuplesOk(result) || PQntuples(result.get()) == 0) return {0, 0};
    return {cellInt64(result.get(), 0, 0, 0), cellInt64(result.get(), 0, 1, 0)};
}

std::pair<long long, long long> advanceScoringVersions(PGconn *connection,
                                                       const std::string &leagueId,
                                                       bool standingsChanged) {
    auto result = execute(connection,
        "INSERT INTO scoring_states (league_id, version, standings_version, updated_at) "
        "VALUES ($1, 1, CASE WHEN $2::boolean THEN 1 ELSE 0 END, NOW()) "
        "ON CONFLICT (league_id) DO UPDATE SET "
        "version = scoring_states.version + 1, "
        "standings_version = scoring_states.standings_version + CASE WHEN $2::boolean THEN 1 ELSE 0 END, "
        "updated_at = NOW() RETURNING version, standings_version",
        {leagueId, standingsChanged ? "true" : "false"});
    if (!tuplesOk(result) || PQntuples(result.get()) == 0) return {-1, -1};
    return {cellInt64(result.get(), 0, 0, -1), cellInt64(result.get(), 0, 1, -1)};
}

bool ensureWeekState(PGconn *connection,
                     const std::string &leagueId,
                     int season,
                     int week) {
    return commandOk(execute(connection,
        "INSERT INTO scoring_week_states (league_id, season, week, version, status, updated_at) "
        "VALUES ($1, $2::int, $3::int, 0, 'unscored', NOW()) "
        "ON CONFLICT (league_id, season, week) DO NOTHING",
        {leagueId, std::to_string(season), std::to_string(week)}));
}

struct WeekRecord {
    long long version{0};
    std::string status{"unscored"};
    std::string inputHash;
    Json::Value scoringSettings{Json::objectValue};
    Json::Value lineupSnapshot{Json::arrayValue};
    Json::Value playerScores{Json::arrayValue};
    Json::Value matchups{Json::arrayValue};
    std::string scoredAt;
    std::string finalizedAt;
    std::string finalizedBy;
};

WeekRecord weekRecord(PGconn *connection,
                      const std::string &leagueId,
                      int season,
                      int week) {
    WeekRecord record;
    auto result = execute(connection,
        "SELECT version, status, input_hash, scoring_settings_snapshot::text, "
        "lineup_snapshot::text, player_scores_snapshot::text, matchup_snapshot::text, "
        "COALESCE(to_char(scored_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), ''), "
        "COALESCE(to_char(finalized_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), ''), "
        "COALESCE(finalized_by_email, '') "
        "FROM scoring_week_states WHERE league_id = $1 AND season = $2::int AND week = $3::int LIMIT 1",
        {leagueId, std::to_string(season), std::to_string(week)});
    if (!tuplesOk(result) || PQntuples(result.get()) == 0) return record;
    record.version = cellInt64(result.get(), 0, 0, 0);
    record.status = cell(result.get(), 0, 1);
    record.inputHash = cell(result.get(), 0, 2);
    record.scoringSettings = jsonFromString(cell(result.get(), 0, 3));
    record.lineupSnapshot = jsonFromString(cell(result.get(), 0, 4), Json::Value{Json::arrayValue});
    record.playerScores = jsonFromString(cell(result.get(), 0, 5), Json::Value{Json::arrayValue});
    record.matchups = jsonFromString(cell(result.get(), 0, 6), Json::Value{Json::arrayValue});
    record.scoredAt = cell(result.get(), 0, 7);
    record.finalizedAt = cell(result.get(), 0, 8);
    record.finalizedBy = canonicalEmail(cell(result.get(), 0, 9));
    return record;
}

Json::Value activeMembersPayload(PGconn *connection, const std::string &leagueId) {
    auto result = execute(connection,
        "SELECT lower(email), COALESCE(team_name, ''), role, status, "
        "COALESCE(to_char(COALESCE(joined_at, created_at) AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), '') "
        "FROM league_members WHERE league_id = $1 AND status = 'active' "
        "ORDER BY COALESCE(joined_at, created_at), lower(email)",
        {leagueId});
    Json::Value members(Json::arrayValue);
    if (!tuplesOk(result)) return members;
    for (int row = 0; row < PQntuples(result.get()); ++row) {
        Json::Value member(Json::objectValue);
        member["email"] = canonicalEmail(cell(result.get(), row, 0));
        member["teamName"] = cell(result.get(), row, 1);
        member["role"] = cell(result.get(), row, 2);
        member["status"] = "Active";
        member["joinedAt"] = cell(result.get(), row, 4);
        members.append(member);
    }
    return members;
}

Json::Value lineupSnapshotPayload(PGconn *connection, const std::string &leagueId) {
    auto result = execute(connection,
        "SELECT lower(manager_email), player_id, player_snapshot::text, lower(roster_slot) "
        "FROM rosters WHERE league_id = $1 AND lower(roster_slot) <> 'bench' "
        "ORDER BY lower(manager_email), lower(roster_slot), player_id",
        {leagueId});
    Json::Value lineup(Json::arrayValue);
    if (!tuplesOk(result)) return lineup;
    for (int row = 0; row < PQntuples(result.get()); ++row) {
        auto player = jsonFromString(cell(result.get(), row, 2));
        if (!player.isObject()) player = Json::Value{Json::objectValue};
        player["id"] = cell(result.get(), row, 1);
        player["playerId"] = cell(result.get(), row, 1);
        Json::Value entry(Json::objectValue);
        entry["managerEmail"] = canonicalEmail(cell(result.get(), row, 0));
        entry["playerId"] = cell(result.get(), row, 1);
        entry["rosterSlot"] = cell(result.get(), row, 3);
        entry["player"] = player;
        lineup.append(entry);
    }
    return lineup;
}

Json::Value lineupErrorsPayload(const Json::Value &members,
                                const Json::Value &lineup,
                                const Json::Value &rosterRules) {
    Json::Value errors(Json::arrayValue);
    for (const auto &member : members) {
        const auto email = canonicalEmail(member.get("email", "").asString());
        std::unordered_map<std::string, int> counts;
        for (const auto &entry : lineup) {
            if (canonicalEmail(entry.get("managerEmail", "").asString()) != email) continue;
            auto slot = lower(entry.get("rosterSlot", "bench").asString());
            if (slot != "bench") ++counts[slot];
        }
        const auto managerErrors = cff::league_roster::lineupErrorsFromCounts(email, rosterRules, counts);
        for (const auto &error : managerErrors) errors.append(error);
    }
    return errors;
}

struct ScoreInputs {
    Json::Value lineup{Json::arrayValue};
    Json::Value playerScores{Json::arrayValue};
    std::unordered_map<std::string, double> managerTotals;
};

ScoreInputs scoreInputs(PGconn *connection,
                        const std::string &leagueId,
                        int season,
                        int week,
                        const Json::Value &scoringSettings) {
    ScoreInputs inputs;
    inputs.lineup = lineupSnapshotPayload(connection, leagueId);
    auto result = execute(connection,
        "SELECT lower(r.manager_email), r.player_id, r.player_snapshot::text, lower(r.roster_slot), "
        "COALESCE(ps.category, ''), COALESCE(ps.stat_name, ''), COALESCE(ps.stat_value, 0) "
        "FROM rosters r LEFT JOIN player_stats ps "
        "ON ps.player_id = r.player_id AND ps.season = $2::int AND ps.week = $3::int "
        "WHERE r.league_id = $1 AND lower(r.roster_slot) <> 'bench' "
        "ORDER BY lower(r.manager_email), r.player_id, lower(COALESCE(ps.category, '')), lower(COALESCE(ps.stat_name, ''))",
        {leagueId, std::to_string(season), std::to_string(week)});
    if (!tuplesOk(result)) return inputs;

    struct PlayerAccumulator {
        std::string manager;
        std::string playerId;
        Json::Value player{Json::objectValue};
        std::string rosterSlot;
        Json::Value stats{Json::objectValue};
        double points{0.0};
    };
    std::vector<std::string> order;
    std::unordered_map<std::string, PlayerAccumulator> players;
    for (int row = 0; row < PQntuples(result.get()); ++row) {
        const auto manager = canonicalEmail(cell(result.get(), row, 0));
        const auto playerId = cell(result.get(), row, 1);
        const auto key = manager + "\n" + playerId;
        if (!players.count(key)) {
            PlayerAccumulator accumulator;
            accumulator.manager = manager;
            accumulator.playerId = playerId;
            accumulator.player = jsonFromString(cell(result.get(), row, 2));
            if (!accumulator.player.isObject()) accumulator.player = Json::Value{Json::objectValue};
            accumulator.player["id"] = playerId;
            accumulator.player["playerId"] = playerId;
            accumulator.rosterSlot = cell(result.get(), row, 3);
            players.emplace(key, accumulator);
            order.push_back(key);
        }
        const auto category = cell(result.get(), row, 4);
        const auto statName = cell(result.get(), row, 5);
        const auto value = cellDouble(result.get(), row, 6, 0.0);
        if (!category.empty() && !statName.empty()) {
            players[key].stats[category + "." + statName] = value;
            players[key].points += cff::scoring_lifecycle::fantasyPointsForStat(
                scoringSettings, category, statName, value);
        }
    }

    for (const auto &key : order) {
        const auto &player = players[key];
        Json::Value score(Json::objectValue);
        score["managerEmail"] = player.manager;
        score["playerId"] = player.playerId;
        score["player"] = player.player;
        score["rosterSlot"] = player.rosterSlot;
        score["fantasyPoints"] = player.points;
        score["stats"] = player.stats;
        inputs.playerScores.append(score);
        inputs.managerTotals[player.manager] += player.points;
    }
    return inputs;
}

Json::Value existingWeekMatchups(PGconn *connection,
                                 const std::string &leagueId,
                                 int season,
                                 int week) {
    auto result = execute(connection,
        "SELECT id, week, lower(home_manager_email), lower(COALESCE(away_manager_email, '')), "
        "home_score, away_score, status, "
        "COALESCE(to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), ''), "
        "COALESCE(to_char(finalized_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), '') "
        "FROM league_matchups WHERE league_id = $1 AND week = $3::int "
        "AND (season = $2::int OR season = 0) ORDER BY id",
        {leagueId, std::to_string(season), std::to_string(week)});
    Json::Value matchups(Json::arrayValue);
    if (!tuplesOk(result)) return matchups;
    for (int row = 0; row < PQntuples(result.get()); ++row) {
        Json::Value matchup(Json::objectValue);
        matchup["id"] = cell(result.get(), row, 0);
        matchup["leagueId"] = leagueId;
        matchup["season"] = season;
        matchup["week"] = std::stoi(cell(result.get(), row, 1));
        matchup["homeManager"] = canonicalEmail(cell(result.get(), row, 2));
        matchup["awayManager"] = canonicalEmail(cell(result.get(), row, 3));
        matchup["homeScore"] = cellDouble(result.get(), row, 4);
        matchup["awayScore"] = cellDouble(result.get(), row, 5);
        matchup["status"] = cell(result.get(), row, 6);
        matchup["createdAt"] = cell(result.get(), row, 7);
        matchup["finalizedAt"] = cell(result.get(), row, 8);
        matchups.append(matchup);
    }
    return matchups;
}

Json::Value seasonFinalMatchups(PGconn *connection,
                                const std::string &leagueId,
                                int season) {
    auto result = execute(connection,
        "SELECT id, week, lower(home_manager_email), lower(COALESCE(away_manager_email, '')), "
        "home_score, away_score, status, "
        "COALESCE(to_char(finalized_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), '') "
        "FROM league_matchups WHERE league_id = $1 AND season = $2::int AND status = 'final' "
        "ORDER BY week, id",
        {leagueId, std::to_string(season)});
    Json::Value matchups(Json::arrayValue);
    if (!tuplesOk(result)) return matchups;
    for (int row = 0; row < PQntuples(result.get()); ++row) {
        Json::Value matchup(Json::objectValue);
        matchup["id"] = cell(result.get(), row, 0);
        matchup["leagueId"] = leagueId;
        matchup["season"] = season;
        matchup["week"] = std::stoi(cell(result.get(), row, 1));
        matchup["homeManager"] = canonicalEmail(cell(result.get(), row, 2));
        matchup["awayManager"] = canonicalEmail(cell(result.get(), row, 3));
        matchup["homeScore"] = cellDouble(result.get(), row, 4);
        matchup["awayScore"] = cellDouble(result.get(), row, 5);
        matchup["status"] = cell(result.get(), row, 6);
        matchup["finalizedAt"] = cell(result.get(), row, 7);
        matchups.append(matchup);
    }
    return matchups;
}

Json::Value standingsPayload(PGconn *connection,
                             const std::string &leagueId,
                             int season) {
    auto result = execute(connection,
        "SELECT s.rank, lower(s.manager_email), COALESCE(m.team_name, ''), "
        "COALESCE(m.role, 'member'), COALESCE(m.status, 'active'), "
        "s.wins, s.losses, s.ties, s.games_played, s.points_for, s.points_against, s.win_pct, s.standings_version "
        "FROM league_standings s LEFT JOIN league_members m "
        "ON m.league_id = s.league_id AND lower(m.email) = lower(s.manager_email) "
        "WHERE s.league_id = $1 AND s.season = $2::int ORDER BY s.rank, lower(s.manager_email)",
        {leagueId, std::to_string(season)});
    Json::Value standings(Json::arrayValue);
    if (!tuplesOk(result)) return standings;
    for (int row = 0; row < PQntuples(result.get()); ++row) {
        Json::Value item(Json::objectValue);
        item["rank"] = std::stoi(cell(result.get(), row, 0));
        item["email"] = canonicalEmail(cell(result.get(), row, 1));
        item["managerEmail"] = canonicalEmail(cell(result.get(), row, 1));
        item["teamName"] = cell(result.get(), row, 2);
        item["role"] = cell(result.get(), row, 3);
        item["status"] = cell(result.get(), row, 4) == "active" ? "Active" : cell(result.get(), row, 4);
        item["wins"] = std::stoi(cell(result.get(), row, 5));
        item["losses"] = std::stoi(cell(result.get(), row, 6));
        item["ties"] = std::stoi(cell(result.get(), row, 7));
        item["gamesPlayed"] = std::stoi(cell(result.get(), row, 8));
        item["pointsFor"] = cellDouble(result.get(), row, 9);
        item["pointsAgainst"] = cellDouble(result.get(), row, 10);
        item["winPct"] = cellDouble(result.get(), row, 11);
        item["standingsVersion"] = Json::Int64(cellInt64(result.get(), row, 12));
        standings.append(item);
    }
    return standings;
}

bool replaceStandings(PGconn *connection,
                      const std::string &leagueId,
                      int season,
                      long long standingsVersion,
                      const Json::Value &standings) {
    if (!commandOk(execute(connection,
        "DELETE FROM league_standings WHERE league_id = $1 AND season = $2::int",
        {leagueId, std::to_string(season)}))) return false;
    for (const auto &row : standings) {
        if (!commandOk(execute(connection,
            "INSERT INTO league_standings "
            "(league_id, season, manager_email, rank, wins, losses, ties, games_played, "
            "points_for, points_against, win_pct, standings_version, updated_at) "
            "VALUES ($1, $2::int, $3, $4::int, $5::int, $6::int, $7::int, $8::int, "
            "$9::numeric, $10::numeric, $11::numeric, $12::bigint, NOW())",
            {leagueId, std::to_string(season), canonicalEmail(row.get("managerEmail", row.get("email", "")).asString()),
             std::to_string(row.get("rank", 0).asInt()), std::to_string(row.get("wins", 0).asInt()),
             std::to_string(row.get("losses", 0).asInt()), std::to_string(row.get("ties", 0).asInt()),
             std::to_string(row.get("gamesPlayed", 0).asInt()), std::to_string(row.get("pointsFor", 0.0).asDouble()),
             std::to_string(row.get("pointsAgainst", 0.0).asDouble()), std::to_string(row.get("winPct", 0.0).asDouble()),
             std::to_string(standingsVersion)}))) return false;
    }
    return true;
}

std::string inputHash(PGconn *connection, const Json::Value &input) {
    auto result = execute(connection, "SELECT md5($1)", {jsonToString(input)});
    return tuplesOk(result) && PQntuples(result.get()) > 0 ? cell(result.get(), 0, 0) : "";
}

std::optional<Json::Value> scoringOperationReplay(PGconn *connection,
                                                  const std::string &leagueId,
                                                  const std::string &email,
                                                  const std::string &key,
                                                  const std::string &operationType) {
    if (key.empty()) return std::nullopt;
    auto result = execute(connection,
        "SELECT operation_type, response_payload::text FROM scoring_operations "
        "WHERE league_id = $1 AND lower(actor_email) = lower($2) AND operation_key = $3 LIMIT 1",
        {leagueId, email, key});
    if (!tuplesOk(result) || PQntuples(result.get()) == 0) return std::nullopt;
    auto payload = jsonFromString(cell(result.get(), 0, 1));
    payload["storedOperationType"] = cell(result.get(), 0, 0);
    payload["operationTypeMatches"] = cell(result.get(), 0, 0) == operationType;
    payload["idempotentReplay"] = true;
    return payload;
}

bool recordScoringOperation(PGconn *connection,
                            const std::string &leagueId,
                            const std::string &email,
                            const std::string &key,
                            const std::string &type,
                            int season,
                            int week,
                            long long version,
                            long long standingsVersion,
                            const Json::Value &payload) {
    if (key.empty()) return true;
    return commandOk(execute(connection,
        "INSERT INTO scoring_operations "
        "(league_id, actor_email, operation_key, operation_type, season, week, resulting_version, "
        "resulting_standings_version, response_payload, created_at) "
        "VALUES ($1, $2, $3, $4, $5::int, $6::int, $7::bigint, $8::bigint, $9::jsonb, NOW()) "
        "ON CONFLICT (league_id, actor_email, operation_key) DO NOTHING",
        {leagueId, email, key, type, std::to_string(season), std::to_string(week),
         std::to_string(version), std::to_string(standingsVersion), jsonToString(payload)}));
}

bool addScoringTransaction(PGconn *connection,
                           const std::string &leagueId,
                           const std::string &email,
                           const std::string &operationKeyValue,
                           const std::string &type,
                           const std::string &summary,
                           const Json::Value &metadata) {
    const auto suffix = operationKeyValue.empty()
        ? std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
        : operationKeyValue;
    return commandOk(execute(connection,
        "INSERT INTO transactions (id, league_id, manager_email, transaction_type, summary, metadata, created_at) "
        "VALUES ($1, $2, $3, $4, $5, $6::jsonb, NOW()) ON CONFLICT (id) DO NOTHING",
        {"scoring-" + leagueId + "-" + suffix, leagueId, email, type, summary, jsonToString(metadata)}));
}
