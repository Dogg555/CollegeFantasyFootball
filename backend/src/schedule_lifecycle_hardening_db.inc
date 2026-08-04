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
    return static_cast<int>(cellInt64(result, row, column, fallback));
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

bool lockScheduleLeague(PGconn *connection, const std::string &leagueId) {
    return tuplesOk(execute(connection,
        "SELECT pg_advisory_xact_lock(hashtextextended($1, 0))",
        {"schedule:" + leagueId}));
}

struct ScheduleAccess {
    bool exists{false};
    bool member{false};
    bool commissioner{false};
};

ScheduleAccess scheduleAccess(PGconn *connection,
                              const std::string &leagueId,
                              const std::string &email) {
    ScheduleAccess access;
    auto result = execute(connection,
        "SELECT lower(l.account_email), COALESCE(lower(lm.email), ''), "
        "COALESCE(lm.role, ''), COALESCE(lm.status, '') "
        "FROM leagues l LEFT JOIN league_members lm "
        "ON lm.league_id = l.id AND lower(lm.email) = lower($2) "
        "WHERE l.id = $1 LIMIT 1",
        {leagueId, email});
    if (!tuplesOk(result) || PQntuples(result.get()) == 0) return access;
    access.exists = true;
    const auto owner = canonicalEmail(cell(result.get(), 0, 0));
    const auto member = canonicalEmail(cell(result.get(), 0, 1));
    const auto role = lower(cell(result.get(), 0, 2));
    const auto status = lower(cell(result.get(), 0, 3));
    access.member = owner == email || (member == email && status == "active");
    access.commissioner = owner == email || (member == email && role == "commissioner" && status == "active");
    return access;
}

bool ensureScheduleState(PGconn *connection,
                         const std::string &leagueId,
                         int season) {
    return commandOk(execute(connection,
        "INSERT INTO league_schedule_states "
        "(league_id, season, weeks, current_week, version, schedule_hash, updated_at) "
        "VALUES ($1, $2::int, 12, 1, 0, '', NOW()) "
        "ON CONFLICT (league_id) DO NOTHING",
        {leagueId, std::to_string(season)}));
}

struct ScheduleStateRecord {
    int season{0};
    int weeks{12};
    int currentWeek{1};
    long long version{0};
    std::string scheduleHash;
    std::string generatedBy;
    std::string generatedAt;
};

ScheduleStateRecord scheduleStateRecord(PGconn *connection,
                                        const std::string &leagueId) {
    ScheduleStateRecord state;
    auto result = execute(connection,
        "SELECT season, weeks, current_week, version, schedule_hash, "
        "COALESCE(generated_by_email, ''), "
        "COALESCE(to_char(generated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), '') "
        "FROM league_schedule_states WHERE league_id = $1 LIMIT 1",
        {leagueId});
    if (!tuplesOk(result) || PQntuples(result.get()) == 0) return state;
    state.season = cellInt(result.get(), 0, 0, 0);
    state.weeks = cellInt(result.get(), 0, 1, 12);
    state.currentWeek = cellInt(result.get(), 0, 2, 1);
    state.version = cellInt64(result.get(), 0, 3, 0);
    state.scheduleHash = cell(result.get(), 0, 4);
    state.generatedBy = canonicalEmail(cell(result.get(), 0, 5));
    state.generatedAt = cell(result.get(), 0, 6);
    return state;
}

Json::Value activeMembersPayload(PGconn *connection, const std::string &leagueId) {
    auto result = execute(connection,
        "SELECT lower(email), COALESCE(team_name, ''), role, status "
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
        members.append(member);
    }
    return members;
}

Json::Value currentLineupSnapshot(PGconn *connection, const std::string &leagueId) {
    auto result = execute(connection,
        "SELECT lower(manager_email), player_id, lower(roster_slot), player_snapshot::text "
        "FROM rosters WHERE league_id = $1 AND lower(roster_slot) <> 'bench' "
        "ORDER BY lower(manager_email), lower(roster_slot), player_id",
        {leagueId});
    Json::Value lineup(Json::arrayValue);
    if (!tuplesOk(result)) return lineup;
    for (int row = 0; row < PQntuples(result.get()); ++row) {
        Json::Value entry(Json::objectValue);
        entry["managerEmail"] = canonicalEmail(cell(result.get(), row, 0));
        entry["playerId"] = cell(result.get(), row, 1);
        entry["rosterSlot"] = cell(result.get(), row, 2);
        entry["player"] = jsonFromString(cell(result.get(), row, 3));
        lineup.append(entry);
    }
    return lineup;
}

bool refreshExpiredDeadlines(PGconn *connection,
                             const std::string &leagueId,
                             int season) {
    const auto lineup = currentLineupSnapshot(connection, leagueId);
    return commandOk(execute(connection,
        "UPDATE league_week_controls SET status = 'locked', lineup_version = lineup_version + 1, "
        "lineup_snapshot = $3::jsonb, lineup_hash = md5(($3::jsonb)::text), "
        "locked_at = NOW(), locked_by_email = 'system:deadline', lock_reason = 'deadline', updated_at = NOW() "
        "WHERE league_id = $1 AND season = $2::int AND status = 'open' "
        "AND lineup_deadline IS NOT NULL AND lineup_deadline <= NOW()",
        {leagueId, std::to_string(season), jsonToString(lineup)}));
}

Json::Value scheduleMatchupsPayload(PGconn *connection,
                                    const std::string &leagueId,
                                    int season) {
    auto result = execute(connection,
        "SELECT id, season, week, matchup_key, schedule_slot, lower(home_manager_email), "
        "lower(COALESCE(away_manager_email, '')), home_score, away_score, status, schedule_version, "
        "COALESCE(to_char(finalized_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), '') "
        "FROM league_matchups WHERE league_id = $1 AND (season = $2::int OR season = 0) "
        "ORDER BY week, schedule_slot, id",
        {leagueId, std::to_string(season)});
    Json::Value matchups(Json::arrayValue);
    if (!tuplesOk(result)) return matchups;
    for (int row = 0; row < PQntuples(result.get()); ++row) {
        Json::Value matchup(Json::objectValue);
        matchup["id"] = cell(result.get(), row, 0);
        matchup["leagueId"] = leagueId;
        matchup["season"] = cellInt(result.get(), row, 1, season);
        matchup["week"] = cellInt(result.get(), row, 2, 1);
        matchup["matchupKey"] = cell(result.get(), row, 3);
        matchup["scheduleSlot"] = cellInt(result.get(), row, 4, 0);
        matchup["homeManager"] = canonicalEmail(cell(result.get(), row, 5));
        matchup["awayManager"] = canonicalEmail(cell(result.get(), row, 6));
        matchup["homeScore"] = std::stod(cell(result.get(), row, 7).empty() ? "0" : cell(result.get(), row, 7));
        matchup["awayScore"] = std::stod(cell(result.get(), row, 8).empty() ? "0" : cell(result.get(), row, 8));
        matchup["status"] = cell(result.get(), row, 9);
        matchup["scheduleVersion"] = Json::Int64(cellInt64(result.get(), row, 10, 0));
        matchup["finalizedAt"] = cell(result.get(), row, 11);
        matchups.append(matchup);
    }
    return matchups;
}

Json::Value weekControlsPayload(PGconn *connection,
                                const std::string &leagueId,
                                int season) {
    auto result = execute(connection,
        "SELECT week, schedule_version, lineup_version, status, "
        "COALESCE(to_char(lineup_deadline AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), ''), "
        "lineup_snapshot::text, lineup_hash, "
        "COALESCE(to_char(locked_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), ''), "
        "COALESCE(locked_by_email, ''), COALESCE(lock_reason, ''), "
        "COALESCE(to_char(unlocked_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), ''), "
        "COALESCE(unlocked_by_email, ''), "
        "COALESCE(to_char(finalized_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), '') "
        "FROM league_week_controls WHERE league_id = $1 AND season = $2::int ORDER BY week",
        {leagueId, std::to_string(season)});
    Json::Value controls(Json::arrayValue);
    if (!tuplesOk(result)) return controls;
    for (int row = 0; row < PQntuples(result.get()); ++row) {
        Json::Value control(Json::objectValue);
        control["week"] = cellInt(result.get(), row, 0, 1);
        control["scheduleVersion"] = Json::Int64(cellInt64(result.get(), row, 1, 0));
        control["lineupVersion"] = Json::Int64(cellInt64(result.get(), row, 2, 0));
        control["status"] = cell(result.get(), row, 3);
        control["lineupDeadline"] = cell(result.get(), row, 4);
        control["lineupSnapshot"] = jsonFromString(cell(result.get(), row, 5), Json::Value{Json::arrayValue});
        control["lineupHash"] = cell(result.get(), row, 6);
        control["lockedAt"] = cell(result.get(), row, 7);
        control["lockedByEmail"] = canonicalEmail(cell(result.get(), row, 8));
        control["lockReason"] = cell(result.get(), row, 9);
        control["unlockedAt"] = cell(result.get(), row, 10);
        control["unlockedByEmail"] = canonicalEmail(cell(result.get(), row, 11));
        control["finalizedAt"] = cell(result.get(), row, 12);
        controls.append(control);
    }
    return controls;
}

bool scheduleMutationBlocked(PGconn *connection,
                             const std::string &leagueId,
                             int season) {
    auto result = execute(connection,
        "SELECT EXISTS ("
        "SELECT 1 FROM league_week_controls WHERE league_id = $1 AND season = $2::int "
        "AND status IN ('locked', 'finalized') "
        "UNION ALL "
        "SELECT 1 FROM scoring_week_states WHERE league_id = $1 AND season = $2::int "
        "AND status IN ('scored', 'final')"
        ")",
        {leagueId, std::to_string(season)});
    return tuplesOk(result) && PQntuples(result.get()) > 0 && cell(result.get(), 0, 0) == "t";
}

std::string scoringStatus(PGconn *connection,
                          const std::string &leagueId,
                          int season,
                          int week) {
    auto result = execute(connection,
        "SELECT status FROM scoring_week_states "
        "WHERE league_id = $1 AND season = $2::int AND week = $3::int LIMIT 1",
        {leagueId, std::to_string(season), std::to_string(week)});
    return tuplesOk(result) && PQntuples(result.get()) > 0 ? cell(result.get(), 0, 0) : "unscored";
}

bool matchupFinal(PGconn *connection,
                  const std::string &leagueId,
                  int season,
                  int week) {
    auto result = execute(connection,
        "SELECT EXISTS (SELECT 1 FROM league_matchups WHERE league_id = $1 "
        "AND (season = $2::int OR season = 0) AND week = $3::int AND status = 'final')",
        {leagueId, std::to_string(season), std::to_string(week)});
    return tuplesOk(result) && PQntuples(result.get()) > 0 && cell(result.get(), 0, 0) == "t";
}

std::optional<Json::Value> scheduleOperationReplay(PGconn *connection,
                                                   const std::string &leagueId,
                                                   const std::string &email,
                                                   const std::string &key,
                                                   const std::string &type) {
    if (key.empty()) return std::nullopt;
    auto result = execute(connection,
        "SELECT operation_type, response_payload::text FROM schedule_operations "
        "WHERE league_id = $1 AND actor_email = $2 AND operation_key = $3 LIMIT 1",
        {leagueId, email, key});
    if (!tuplesOk(result) || PQntuples(result.get()) == 0) return std::nullopt;
    auto payload = jsonFromString(cell(result.get(), 0, 1));
    payload["idempotentReplay"] = true;
    payload["operationTypeMatches"] = cell(result.get(), 0, 0) == type;
    payload["storedOperationType"] = cell(result.get(), 0, 0);
    return payload;
}

bool storeScheduleOperation(PGconn *connection,
                            const std::string &leagueId,
                            const std::string &email,
                            const std::string &key,
                            const std::string &type,
                            long long version,
                            const Json::Value &payload) {
    if (key.empty()) return true;
    return commandOk(execute(connection,
        "INSERT INTO schedule_operations "
        "(league_id, actor_email, operation_key, operation_type, resulting_version, response_payload) "
        "VALUES ($1, $2, $3, $4, $5::bigint, $6::jsonb) "
        "ON CONFLICT (league_id, actor_email, operation_key) DO NOTHING",
        {leagueId, email, key, type, std::to_string(version), jsonToString(payload)}));
}

bool currentLineupLocked(PGconn *connection,
                         const std::string &leagueId) {
    auto result = execute(connection, "SELECT cff_current_lineup_locked($1)", {leagueId});
    return tuplesOk(result) && PQntuples(result.get()) > 0 && cell(result.get(), 0, 0) == "t";
}
