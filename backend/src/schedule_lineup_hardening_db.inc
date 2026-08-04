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
    try { return std::stoll(value); } catch (...) { return fallback; }
}

int cellInt(PGresult *result, int row, int column, int fallback = 0) {
    const auto value = cell(result, row, column);
    if (value.empty()) return fallback;
    try { return std::stoi(value); } catch (...) { return fallback; }
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

bool lockScheduleLeague(PGconn *connection,
                        const std::string &leagueId,
                        int season) {
    return tuplesOk(execute(connection,
        "SELECT pg_advisory_xact_lock(hashtextextended($1, 0))",
        {"schedule:" + leagueId + ":" + std::to_string(season)}));
}

struct ScheduleAccess {
    bool exists{false};
    bool member{false};
    bool commissioner{false};
    Json::Value rosterRules{Json::objectValue};
};

ScheduleAccess scheduleAccess(PGconn *connection,
                              const std::string &leagueId,
                              const std::string &email) {
    ScheduleAccess access;
    auto result = execute(connection,
        "SELECT lower(l.account_email), l.roster_rules::text, "
        "COALESCE(lower(lm.email), ''), COALESCE(lm.role, ''), COALESCE(lm.status, '') "
        "FROM leagues l LEFT JOIN league_members lm "
        "ON lm.league_id = l.id AND lower(lm.email) = lower($2) "
        "WHERE l.id = $1 LIMIT 1",
        {leagueId, email});
    if (!tuplesOk(result) || PQntuples(result.get()) == 0) return access;
    access.exists = true;
    const auto owner = canonicalEmail(cell(result.get(), 0, 0));
    const auto memberEmail = canonicalEmail(cell(result.get(), 0, 2));
    const auto role = lower(cell(result.get(), 0, 3));
    const auto status = lower(cell(result.get(), 0, 4));
    access.member = owner == email || (memberEmail == email && status == "active");
    access.commissioner = owner == email
        || (memberEmail == email && role == "commissioner" && status == "active");
    access.rosterRules = jsonFromString(cell(result.get(), 0, 1));
    return access;
}

bool ensureScheduleState(PGconn *connection,
                         const std::string &leagueId,
                         int season) {
    return commandOk(execute(connection,
        "INSERT INTO schedule_states (league_id, season, version, updated_at) "
        "VALUES ($1, $2::int, 0, NOW()) ON CONFLICT (league_id, season) DO NOTHING",
        {leagueId, std::to_string(season)}));
}

struct ScheduleRecord {
    long long version{0};
    std::string inputHash;
    int weeks{0};
    Json::Value managerOrder{Json::arrayValue};
    Json::Value schedule{Json::arrayValue};
};

ScheduleRecord scheduleRecord(PGconn *connection,
                              const std::string &leagueId,
                              int season) {
    ScheduleRecord record;
    auto result = execute(connection,
        "SELECT version, input_hash, weeks, manager_order::text, schedule_snapshot::text "
        "FROM schedule_states WHERE league_id = $1 AND season = $2::int LIMIT 1",
        {leagueId, std::to_string(season)});
    if (!tuplesOk(result) || PQntuples(result.get()) == 0) return record;
    record.version = cellInt64(result.get(), 0, 0, 0);
    record.inputHash = cell(result.get(), 0, 1);
    record.weeks = cellInt(result.get(), 0, 2, 0);
    record.managerOrder = jsonFromString(cell(result.get(), 0, 3), Json::Value{Json::arrayValue});
    record.schedule = jsonFromString(cell(result.get(), 0, 4), Json::Value{Json::arrayValue});
    return record;
}

Json::Value activeMembersPayload(PGconn *connection,
                                 const std::string &leagueId) {
    auto result = execute(connection,
        "SELECT lower(email), COALESCE(team_name, ''), role, status, "
        "COALESCE(to_char(COALESCE(joined_at, created_at) AT TIME ZONE 'UTC', "
        "'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), '') "
        "FROM league_members WHERE league_id = $1 AND status = 'active' "
        "ORDER BY lower(email)",
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

bool ensureWeekState(PGconn *connection,
                     const std::string &leagueId,
                     int season,
                     int week) {
    return commandOk(execute(connection,
        "INSERT INTO schedule_week_states (league_id, season, week, version, status, updated_at) "
        "VALUES ($1, $2::int, $3::int, 0, 'open', NOW()) "
        "ON CONFLICT (league_id, season, week) DO NOTHING",
        {leagueId, std::to_string(season), std::to_string(week)}));
}

bool ensureLineupStates(PGconn *connection,
                        const std::string &leagueId,
                        int season,
                        int week) {
    return commandOk(execute(connection,
        "INSERT INTO lineup_week_states "
        "(league_id, season, week, manager_email, version, status, updated_at) "
        "SELECT $1, $2::int, $3::int, lower(email), 0, 'open', NOW() "
        "FROM league_members WHERE league_id = $1 AND status = 'active' "
        "ON CONFLICT (league_id, season, week, manager_email) DO NOTHING",
        {leagueId, std::to_string(season), std::to_string(week)}));
}

struct WeekStateRecord {
    long long version{0};
    std::string status{"open"};
    std::string deadline;
};

WeekStateRecord weekStateRecord(PGconn *connection,
                                const std::string &leagueId,
                                int season,
                                int week) {
    WeekStateRecord record;
    auto result = execute(connection,
        "SELECT version, status, COALESCE(to_char(lineup_deadline AT TIME ZONE 'UTC', "
        "'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), '') "
        "FROM schedule_week_states WHERE league_id = $1 AND season = $2::int AND week = $3::int LIMIT 1",
        {leagueId, std::to_string(season), std::to_string(week)});
    if (!tuplesOk(result) || PQntuples(result.get()) == 0) return record;
    record.version = cellInt64(result.get(), 0, 0, 0);
    record.status = cell(result.get(), 0, 1);
    record.deadline = cell(result.get(), 0, 2);
    return record;
}

std::string scoringWeekStatus(PGconn *connection,
                              const std::string &leagueId,
                              int season,
                              int week) {
    auto result = execute(connection,
        "SELECT status FROM scoring_week_states "
        "WHERE league_id = $1 AND season = $2::int AND week = $3::int LIMIT 1",
        {leagueId, std::to_string(season), std::to_string(week)});
    if (!tuplesOk(result) || PQntuples(result.get()) == 0) return "unscored";
    return lower(cell(result.get(), 0, 0));
}

Json::Value currentLineupSnapshot(PGconn *connection,
                                  const std::string &leagueId,
                                  const std::string &managerEmail) {
    auto result = execute(connection,
        "SELECT player_id, player_snapshot::text, lower(roster_slot) "
        "FROM rosters WHERE league_id = $1 AND lower(manager_email) = lower($2) "
        "AND lower(roster_slot) <> 'bench' ORDER BY lower(roster_slot), player_id",
        {leagueId, managerEmail});
    Json::Value lineup(Json::arrayValue);
    if (!tuplesOk(result)) return lineup;
    for (int row = 0; row < PQntuples(result.get()); ++row) {
        auto player = jsonFromString(cell(result.get(), row, 1));
        if (!player.isObject()) player = Json::Value{Json::objectValue};
        const auto playerId = cell(result.get(), row, 0);
        player["id"] = playerId;
        player["playerId"] = playerId;
        Json::Value entry(Json::objectValue);
        entry["managerEmail"] = canonicalEmail(managerEmail);
        entry["playerId"] = playerId;
        entry["rosterSlot"] = lower(cell(result.get(), row, 2));
        entry["player"] = player;
        lineup.append(entry);
    }
    return lineup;
}

Json::Value lineupErrors(const std::string &managerEmail,
                         const Json::Value &lineup,
                         const Json::Value &rosterRules) {
    std::unordered_map<std::string, int> counts;
    for (const auto &entry : lineup) {
        const auto slot = lower(entry.get("rosterSlot", "bench").asString());
        if (slot != "bench") ++counts[slot];
    }
    return cff::league_roster::lineupErrorsFromCounts(
        canonicalEmail(managerEmail), rosterRules, counts);
}

Json::Value lineupStatesPayload(PGconn *connection,
                                const std::string &leagueId,
                                int season,
                                int week) {
    auto result = execute(connection,
        "SELECT lower(manager_email), version, status, lock_reason, lineup_snapshot::text, "
        "validation_errors::text, "
        "COALESCE(to_char(locked_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), ''), "
        "COALESCE(to_char(unlocked_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), '') "
        "FROM lineup_week_states WHERE league_id = $1 AND season = $2::int AND week = $3::int "
        "ORDER BY lower(manager_email)",
        {leagueId, std::to_string(season), std::to_string(week)});
    Json::Value lineups(Json::arrayValue);
    if (!tuplesOk(result)) return lineups;
    for (int row = 0; row < PQntuples(result.get()); ++row) {
        Json::Value lineup(Json::objectValue);
        lineup["managerEmail"] = canonicalEmail(cell(result.get(), row, 0));
        lineup["version"] = Json::Int64(cellInt64(result.get(), row, 1, 0));
        lineup["status"] = cell(result.get(), row, 2);
        lineup["lockReason"] = cell(result.get(), row, 3);
        lineup["lineup"] = jsonFromString(cell(result.get(), row, 4), Json::Value{Json::arrayValue});
        lineup["validationErrors"] = jsonFromString(cell(result.get(), row, 5), Json::Value{Json::arrayValue});
        lineup["lockedAt"] = cell(result.get(), row, 6);
        lineup["unlockedAt"] = cell(result.get(), row, 7);
        lineups.append(lineup);
    }
    return lineups;
}

bool allManagersLocked(PGconn *connection,
                       const std::string &leagueId,
                       int season,
                       int week) {
    auto result = execute(connection,
        "SELECT COUNT(*) FILTER (WHERE member.status = 'active'), "
        "COUNT(*) FILTER (WHERE member.status = 'active' AND lineup.status IN ('locked', 'finalized')) "
        "FROM league_members member LEFT JOIN lineup_week_states lineup "
        "ON lineup.league_id = member.league_id AND lower(lineup.manager_email) = lower(member.email) "
        "AND lineup.season = $2::int AND lineup.week = $3::int "
        "WHERE member.league_id = $1",
        {leagueId, std::to_string(season), std::to_string(week)});
    if (!tuplesOk(result) || PQntuples(result.get()) == 0) return false;
    const auto active = cellInt(result.get(), 0, 0, 0);
    const auto locked = cellInt(result.get(), 0, 1, 0);
    return active > 0 && active == locked;
}

bool managerHasActiveLineupLock(PGconn *connection,
                                const std::string &leagueId,
                                const std::string &managerEmail) {
    auto result = execute(connection,
        "SELECT EXISTS (SELECT 1 FROM lineup_week_states lineup "
        "LEFT JOIN scoring_week_states scoring ON scoring.league_id = lineup.league_id "
        "AND scoring.season = lineup.season AND scoring.week = lineup.week "
        "WHERE lineup.league_id = $1 AND lower(lineup.manager_email) = lower($2) "
        "AND lineup.status = 'locked' AND COALESCE(scoring.status, 'unscored') <> 'final')",
        {leagueId, managerEmail});
    return tuplesOk(result) && PQntuples(result.get()) > 0 && cell(result.get(), 0, 0) == "t";
}

bool playerIsLockedStarter(PGconn *connection,
                           const std::string &leagueId,
                           const std::string &managerEmail,
                           const std::string &playerId) {
    auto result = execute(connection,
        "SELECT EXISTS (SELECT 1 FROM rosters roster "
        "WHERE roster.league_id = $1 AND lower(roster.manager_email) = lower($2) "
        "AND roster.player_id = $3 AND lower(roster.roster_slot) <> 'bench')",
        {leagueId, managerEmail, playerId});
    return tuplesOk(result) && PQntuples(result.get()) > 0 && cell(result.get(), 0, 0) == "t"
        && managerHasActiveLineupLock(connection, leagueId, managerEmail);
}

bool scheduleProtected(PGconn *connection,
                       const std::string &leagueId,
                       int season) {
    auto result = execute(connection,
        "SELECT EXISTS ("
        "SELECT 1 FROM scoring_week_states WHERE league_id = $1 AND season = $2::int AND status <> 'unscored' "
        "UNION ALL SELECT 1 FROM lineup_week_states WHERE league_id = $1 AND season = $2::int AND status <> 'open' "
        "UNION ALL SELECT 1 FROM league_matchups WHERE league_id = $1 AND season = $2::int AND status = 'final'"
        ")",
        {leagueId, std::to_string(season)});
    return tuplesOk(result) && PQntuples(result.get()) > 0 && cell(result.get(), 0, 0) == "t";
}

std::optional<Json::Value> operationReplay(PGconn *connection,
                                           const std::string &leagueId,
                                           const std::string &email,
                                           const std::string &key,
                                           const std::string &operationType) {
    if (key.empty()) return std::nullopt;
    auto result = execute(connection,
        "SELECT operation_type, response_payload::text FROM schedule_operations "
        "WHERE league_id = $1 AND lower(actor_email) = lower($2) AND operation_key = $3 LIMIT 1",
        {leagueId, email, key});
    if (!tuplesOk(result) || PQntuples(result.get()) == 0) return std::nullopt;
    auto payload = jsonFromString(cell(result.get(), 0, 1));
    payload["idempotentReplay"] = true;
    payload["operationTypeMatches"] = lower(cell(result.get(), 0, 0)) == lower(operationType);
    return payload;
}

bool storeOperation(PGconn *connection,
                    const std::string &leagueId,
                    int season,
                    const std::string &email,
                    const std::string &key,
                    const std::string &operationType,
                    long long version,
                    const Json::Value &payload) {
    if (key.empty()) return true;
    return commandOk(execute(connection,
        "INSERT INTO schedule_operations "
        "(league_id, season, actor_email, operation_key, operation_type, resulting_version, response_payload) "
        "VALUES ($1, $2::int, lower($3), $4, $5, $6::bigint, $7::jsonb) "
        "ON CONFLICT (league_id, actor_email, operation_key) DO NOTHING",
        {leagueId, std::to_string(season), email, key, operationType,
         std::to_string(version), jsonToString(payload)}));
}

bool addScheduleTransaction(PGconn *connection,
                            const std::string &leagueId,
                            const std::string &email,
                            const std::string &type,
                            const std::string &summary,
                            const Json::Value &metadata = Json::Value{Json::objectValue}) {
    return commandOk(execute(connection,
        "INSERT INTO transactions (id, league_id, manager_email, transaction_type, summary, metadata, created_at) "
        "VALUES ('schedule-' || md5($1 || $2 || $3 || clock_timestamp()::text || random()::text), "
        "$1, lower($2), $3, $4, $5::jsonb, NOW())",
        {leagueId, email, type, summary, jsonToString(metadata)}));
}

bool persistSchedule(PGconn *connection,
                     const std::string &leagueId,
                     int season,
                     long long version,
                     const std::string &inputHash,
                     int weeks,
                     const Json::Value &managerOrder,
                     const Json::Value &schedule) {
    if (!commandOk(execute(connection,
        "DELETE FROM league_matchups WHERE league_id = $1 AND season = $2::int AND status <> 'final'",
        {leagueId, std::to_string(season)}))) return false;

    for (const auto &matchup : schedule) {
        if (!commandOk(execute(connection,
            "INSERT INTO league_matchups "
            "(id, league_id, season, week, home_manager_email, away_manager_email, home_score, away_score, "
            "status, schedule_version, schedule_input_hash, scoring_snapshot_hash, scoring_version, "
            "finalized_at, created_at, updated_at) "
            "VALUES ($1, $2, $3::int, $4::int, lower($5), NULLIF(lower($6), ''), 0, 0, "
            "'scheduled', $7::bigint, $8, '', 0, NULL, NOW(), NOW()) "
            "ON CONFLICT (id) DO UPDATE SET home_manager_email = EXCLUDED.home_manager_email, "
            "away_manager_email = EXCLUDED.away_manager_email, schedule_version = EXCLUDED.schedule_version, "
            "schedule_input_hash = EXCLUDED.schedule_input_hash, updated_at = NOW()",
            {matchup.get("id", "").asString(), leagueId, std::to_string(season),
             std::to_string(matchup.get("week", 1).asInt()),
             canonicalEmail(matchup.get("homeManager", "").asString()),
             canonicalEmail(matchup.get("awayManager", "").asString()),
             std::to_string(version), inputHash}))) return false;
    }

    if (!commandOk(execute(connection,
        "DELETE FROM lineup_week_states WHERE league_id = $1 AND season = $2::int",
        {leagueId, std::to_string(season)}))) return false;
    if (!commandOk(execute(connection,
        "DELETE FROM schedule_week_states WHERE league_id = $1 AND season = $2::int",
        {leagueId, std::to_string(season)}))) return false;

    for (int week = 1; week <= weeks; ++week) {
        if (!commandOk(execute(connection,
            "INSERT INTO schedule_week_states (league_id, season, week, version, status, updated_at) "
            "VALUES ($1, $2::int, $3::int, $4::bigint, 'open', NOW())",
            {leagueId, std::to_string(season), std::to_string(week), std::to_string(version)}))) return false;
        if (!ensureLineupStates(connection, leagueId, season, week)) return false;
    }

    auto update = execute(connection,
        "UPDATE schedule_states SET version = $3::bigint, input_hash = $4, weeks = $5::int, "
        "manager_order = $6::jsonb, schedule_snapshot = $7::jsonb, updated_at = NOW() "
        "WHERE league_id = $1 AND season = $2::int",
        {leagueId, std::to_string(season), std::to_string(version), inputHash,
         std::to_string(weeks), jsonToString(managerOrder), jsonToString(schedule)});
    return commandOk(update) && std::string{PQcmdTuples(update.get())} == "1";
}
