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

bool begin(PGconn *connection) {
    return commandOk(execute(connection, "BEGIN"));
}

bool commit(PGconn *connection) {
    return commandOk(execute(connection, "COMMIT"));
}

void rollback(PGconn *connection) {
    (void)execute(connection, "ROLLBACK");
}

bool lockRosterLeague(PGconn *connection, const std::string &leagueId) {
    return tuplesOk(execute(connection,
                            "SELECT pg_advisory_xact_lock(hashtextextended($1, 0))",
                            {"roster:" + leagueId}));
}

struct LeagueAccess {
    bool exists{false};
    bool member{false};
    Json::Value rosterRules{Json::objectValue};
    Json::Value waiverRules{Json::objectValue};
};

LeagueAccess leagueAccess(PGconn *connection,
                          const std::string &leagueId,
                          const std::string &email) {
    LeagueAccess access;
    auto result = execute(connection,
        "SELECT lower(l.account_email), l.roster_rules::text, l.waiver_rules::text, "
        "COALESCE(lm.status, '') "
        "FROM leagues l LEFT JOIN league_members lm "
        "ON lm.league_id = l.id AND lower(lm.email) = lower($2) "
        "WHERE l.id = $1 LIMIT 1",
        {leagueId, email});
    if (!tuplesOk(result) || PQntuples(result.get()) == 0) return access;
    access.exists = true;
    const auto owner = canonicalEmail(cell(result.get(), 0, 0));
    const auto status = lower(cell(result.get(), 0, 3));
    access.member = owner == email || status == "active";
    access.rosterRules = jsonFromString(cell(result.get(), 0, 1));
    access.waiverRules = jsonFromString(cell(result.get(), 0, 2));
    return access;
}

bool ensureRosterState(PGconn *connection,
                       const std::string &leagueId,
                       const std::string &email) {
    return commandOk(execute(connection,
        "INSERT INTO roster_states (league_id, manager_email, version, updated_at) "
        "VALUES ($1, $2, 0, NOW()) ON CONFLICT (league_id, manager_email) DO NOTHING",
        {leagueId, email}));
}

long long rosterVersion(PGconn *connection,
                        const std::string &leagueId,
                        const std::string &email) {
    auto result = execute(connection,
        "SELECT version FROM roster_states WHERE league_id = $1 AND lower(manager_email) = lower($2) LIMIT 1",
        {leagueId, email});
    return tuplesOk(result) && PQntuples(result.get()) > 0
        ? cellInt64(result.get(), 0, 0, 0)
        : 0;
}

long long advanceRosterVersion(PGconn *connection,
                               const std::string &leagueId,
                               const std::string &email) {
    auto result = execute(connection,
        "INSERT INTO roster_states (league_id, manager_email, version, updated_at) "
        "VALUES ($1, $2, 1, NOW()) "
        "ON CONFLICT (league_id, manager_email) DO UPDATE "
        "SET version = roster_states.version + 1, updated_at = NOW() "
        "RETURNING version",
        {leagueId, email});
    return tuplesOk(result) && PQntuples(result.get()) > 0
        ? cellInt64(result.get(), 0, 0, -1)
        : -1;
}

Json::Value rosterPayload(PGconn *connection,
                          const std::string &leagueId,
                          const std::string &email) {
    auto result = execute(connection,
        "SELECT player_id, player_snapshot::text, roster_slot, acquired_via, "
        "COALESCE(to_char(acquired_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), '') "
        "FROM rosters WHERE league_id = $1 AND lower(manager_email) = lower($2) "
        "ORDER BY acquired_at, player_id",
        {leagueId, email});
    Json::Value roster(Json::arrayValue);
    if (!tuplesOk(result)) return roster;
    for (int row = 0; row < PQntuples(result.get()); ++row) {
        auto player = jsonFromString(cell(result.get(), row, 1));
        if (!player.isObject()) player = Json::Value{Json::objectValue};
        player["id"] = cell(result.get(), row, 0);
        player["playerId"] = cell(result.get(), row, 0);
        player["rosterSlot"] = cell(result.get(), row, 2);
        player["acquiredVia"] = cell(result.get(), row, 3);
        player["acquiredAt"] = cell(result.get(), row, 4);
        roster.append(player);
    }
    return roster;
}

Json::Value unavailablePlayerIds(PGconn *connection,
                                 const std::string &leagueId) {
    auto result = execute(connection,
        "SELECT player_id FROM rosters WHERE league_id = $1 ORDER BY player_id",
        {leagueId});
    Json::Value ids(Json::arrayValue);
    if (!tuplesOk(result)) return ids;
    for (int row = 0; row < PQntuples(result.get()); ++row) ids.append(cell(result.get(), row, 0));
    return ids;
}

bool lineupLocked(PGconn *connection, const std::string &leagueId) {
    auto optionalLock = [&](const std::string &tableName,
                            const std::string &tableReference,
                            const std::string &statusPredicate) {
        auto exists = execute(connection,
            "SELECT to_regclass($1) IS NOT NULL",
            {"public." + tableName});
        if (!tuplesOk(exists) || PQntuples(exists.get()) == 0 || cell(exists.get(), 0, 0) != "t") {
            return false;
        }
        auto result = execute(connection,
            "SELECT 1 FROM " + tableReference + " WHERE league_id = $1 AND " + statusPredicate + " LIMIT 1",
            {leagueId});
        return tuplesOk(result) && PQntuples(result.get()) > 0;
    };

    auto scheduleExists = execute(connection,
        "SELECT to_regclass($1) IS NOT NULL",
        {"public.schedule_week_states"});
    if (tuplesOk(scheduleExists)
        && PQntuples(scheduleExists.get()) > 0
        && cell(scheduleExists.get(), 0, 0) == "t") {
        auto finalizedMatchup = execute(connection,
            "SELECT 1 FROM schedule_week_states week_state "
            "JOIN league_matchups matchup "
            "ON matchup.league_id = week_state.league_id "
            "AND matchup.season = week_state.season "
            "AND matchup.week = week_state.week "
            "WHERE week_state.league_id = $1 "
            "AND week_state.status = 'locked' "
            "AND matchup.status = 'final'",
            {leagueId});
        if (tuplesOk(finalizedMatchup) && PQntuples(finalizedMatchup.get()) > 0) return true;
    }

    return optionalLock("scoring_week_states", "scoring_week_states scoring", "scoring.status = 'final'")
        || optionalLock("lineup_week_states", "lineup_week_states", "status = 'finalized'")
        || optionalLock("schedule_week_states", "schedule_week_states", "status IN ('locked', 'finalized')");
}

std::optional<std::string> playerOwner(PGconn *connection,
                                       const std::string &leagueId,
                                       const std::string &playerId) {
    auto result = execute(connection,
        "SELECT lower(manager_email) FROM rosters "
        "WHERE league_id = $1 AND player_id = $2 LIMIT 1 FOR UPDATE",
        {leagueId, playerId});
    if (!tuplesOk(result) || PQntuples(result.get()) == 0) return std::nullopt;
    return canonicalEmail(cell(result.get(), 0, 0));
}

std::optional<Json::Value> rosterOperationReplay(PGconn *connection,
                                                 const std::string &leagueId,
                                                 const std::string &email,
                                                 const std::string &key,
                                                 const std::string &operationType) {
    if (key.empty()) return std::nullopt;
    auto result = execute(connection,
        "SELECT operation_type, response_payload::text FROM roster_operations "
        "WHERE league_id = $1 AND lower(manager_email) = lower($2) AND operation_key = $3 LIMIT 1",
        {leagueId, email, key});
    if (!tuplesOk(result) || PQntuples(result.get()) == 0) return std::nullopt;
    Json::Value payload = jsonFromString(cell(result.get(), 0, 1));
    payload["storedOperationType"] = cell(result.get(), 0, 0);
    payload["operationTypeMatches"] = cell(result.get(), 0, 0) == operationType;
    payload["idempotentReplay"] = true;
    return payload;
}

bool recordRosterOperation(PGconn *connection,
                           const std::string &leagueId,
                           const std::string &email,
                           const std::string &key,
                           const std::string &type,
                           long long version,
                           const Json::Value &payload) {
    if (key.empty()) return true;
    return commandOk(execute(connection,
        "INSERT INTO roster_operations "
        "(league_id, manager_email, operation_key, operation_type, resulting_version, response_payload, created_at) "
        "VALUES ($1, $2, $3, $4, $5::bigint, $6::jsonb, NOW()) "
        "ON CONFLICT (league_id, manager_email, operation_key) DO NOTHING",
        {leagueId, email, key, type, std::to_string(version), jsonToString(payload)}));
}

bool addTransactionRecord(PGconn *connection,
                          const std::string &leagueId,
                          const std::string &email,
                          const std::string &operationKeyValue,
                          const std::string &type,
                          const std::string &summary,
                          const Json::Value &metadata) {
    const auto suffix = operationKeyValue.empty()
        ? std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
        : operationKeyValue;
    const auto id = "roster-" + leagueId + "-" + email + "-" + suffix;
    return commandOk(execute(connection,
        "INSERT INTO transactions (id, league_id, manager_email, transaction_type, summary, metadata, created_at) "
        "VALUES ($1, $2, $3, $4, $5, $6::jsonb, NOW()) ON CONFLICT (id) DO NOTHING",
        {id, leagueId, email, type, summary, jsonToString(metadata)}));
}
