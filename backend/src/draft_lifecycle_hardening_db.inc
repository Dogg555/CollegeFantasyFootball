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

int cellInt(PGresult *result, int row, int column, int fallback = 0) {
    const auto value = cell(result, row, column);
    if (value.empty()) return fallback;
    try {
        return std::stoi(value);
    } catch (...) {
        return fallback;
    }
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

bool lockDraft(PGconn *connection, const std::string &leagueId) {
    return tuplesOk(execute(connection,
                            "SELECT pg_advisory_xact_lock(hashtextextended($1, 0))",
                            {"draft:" + leagueId}));
}

struct LeagueAccess {
    bool exists{false};
    bool member{false};
    bool commissioner{false};
    bool lobbyOpen{false};
    std::string draftType{"snake"};
    Json::Value rosterRules{Json::objectValue};
};

LeagueAccess leagueAccess(PGconn *connection,
                          const std::string &leagueId,
                          const std::string &email) {
    LeagueAccess access;
    auto result = execute(connection,
        "SELECT l.account_email, l.draft_lobby_open, l.draft_type, l.roster_rules::text, "
        "COALESCE(lm.role, ''), COALESCE(lm.status, '') "
        "FROM leagues l LEFT JOIN league_members lm "
        "ON lm.league_id = l.id AND lower(lm.email) = lower($2) "
        "WHERE l.id = $1 LIMIT 1",
        {leagueId, email});
    if (!tuplesOk(result) || PQntuples(result.get()) == 0) return access;
    access.exists = true;
    const auto owner = cff::draft_lifecycle::canonicalEmail(cell(result.get(), 0, 0));
    const auto role = cell(result.get(), 0, 4);
    const auto status = cell(result.get(), 0, 5);
    access.member = owner == email || status == "active";
    access.commissioner = owner == email || (role == "commissioner" && status == "active");
    access.lobbyOpen = cell(result.get(), 0, 1) == "t";
    access.draftType = cell(result.get(), 0, 2).empty() ? "snake" : cell(result.get(), 0, 2);
    access.rosterRules = jsonFromString(cell(result.get(), 0, 3));
    return access;
}

Json::Value activeManagers(PGconn *connection, const std::string &leagueId) {
    auto result = execute(connection,
        "SELECT lower(email) FROM league_members "
        "WHERE league_id = $1 AND status = 'active' ORDER BY lower(email)",
        {leagueId});
    Json::Value managers(Json::arrayValue);
    if (!tuplesOk(result)) return managers;
    for (int row = 0; row < PQntuples(result.get()); ++row) {
        managers.append(cff::draft_lifecycle::canonicalEmail(cell(result.get(), row, 0)));
    }
    return managers;
}

bool ensureDraftState(PGconn *connection,
                      const std::string &leagueId,
                      const Json::Value &managers) {
    const auto order = cff::draft_lifecycle::canonicalOrder(managers);
    return commandOk(execute(connection,
        "INSERT INTO draft_states (league_id, draft_order) "
        "VALUES ($1, ARRAY(SELECT jsonb_array_elements_text($2::jsonb))) "
        "ON CONFLICT (league_id) DO NOTHING",
        {leagueId, jsonToString(order)}));
}

bool touchPresence(PGconn *connection,
                   const std::string &leagueId,
                   const std::string &email) {
    return commandOk(execute(connection,
        "INSERT INTO draft_readiness (league_id, manager_email, ready, last_seen_at, updated_at) "
        "VALUES ($1, $2, FALSE, NOW(), NOW()) "
        "ON CONFLICT (league_id, manager_email) DO UPDATE "
        "SET last_seen_at = NOW(), updated_at = NOW()",
        {leagueId, email}));
}

Json::Value readinessPayload(PGconn *connection, const std::string &leagueId) {
    auto result = execute(connection,
        "SELECT lower(lm.email), lm.role, COALESCE(lm.team_name, ''), "
        "COALESCE(dr.ready, FALSE), "
        "COALESCE(dr.last_seen_at >= NOW() - ($2::integer * INTERVAL '1 second'), FALSE), "
        "COALESCE(to_char(dr.last_seen_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), '') "
        "FROM league_members lm LEFT JOIN draft_readiness dr "
        "ON dr.league_id = lm.league_id AND lower(dr.manager_email) = lower(lm.email) "
        "WHERE lm.league_id = $1 AND lm.status = 'active' ORDER BY lower(lm.email)",
        {leagueId, std::to_string(kPresenceWindowSeconds)});
    Json::Value readiness(Json::arrayValue);
    if (!tuplesOk(result)) return readiness;
    for (int row = 0; row < PQntuples(result.get()); ++row) {
        Json::Value entry(Json::objectValue);
        entry["email"] = cell(result.get(), row, 0);
        entry["role"] = cell(result.get(), row, 1);
        entry["teamName"] = cell(result.get(), row, 2);
        entry["ready"] = cell(result.get(), row, 3) == "t";
        entry["connected"] = cell(result.get(), row, 4) == "t";
        entry["lastSeenAt"] = cell(result.get(), row, 5);
        readiness.append(entry);
    }
    return readiness;
}

std::optional<std::string> operationReplay(PGconn *connection,
                                           const std::string &leagueId,
                                           const std::string &key) {
    if (key.empty()) return std::nullopt;
    auto result = execute(connection,
        "SELECT operation_type FROM draft_operations "
        "WHERE league_id = $1 AND operation_key = $2 LIMIT 1",
        {leagueId, key});
    if (!tuplesOk(result) || PQntuples(result.get()) == 0) return std::nullopt;
    return cell(result.get(), 0, 0);
}

bool recordOperation(PGconn *connection,
                     const std::string &leagueId,
                     const std::string &key,
                     const std::string &type,
                     long long version) {
    if (key.empty()) return true;
    return commandOk(execute(connection,
        "INSERT INTO draft_operations (league_id, operation_key, operation_type, resulting_version) "
        "VALUES ($1, $2, $3, $4::bigint) ON CONFLICT (league_id, operation_key) DO NOTHING",
        {leagueId, key, type, std::to_string(version)}));
}
