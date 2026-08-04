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
    const auto value = cff::config::readEnv("DB_URL");
    return value && !value->empty();
}

PgConnection connectDb() {
    const auto value = cff::config::readEnv("DB_URL");
    if (!value) return nullptr;
    PgConnection connection{PQconnectdb(value->c_str())};
    if (!connection || PQstatus(connection.get()) != CONNECTION_OK) {
        std::cerr << "[stat-ingest] database connection failed: "
                  << (connection ? PQerrorMessage(connection.get()) : "no connection") << std::endl;
        return nullptr;
    }
    return connection;
}

PgResult execute(PGconn *connection,
                 const std::string &sql,
                 const std::vector<std::string> &parameters = {}) {
    std::vector<const char *> values;
    values.reserve(parameters.size());
    for (const auto &parameter : parameters) values.push_back(parameter.c_str());
    PgResult result{PQexecParams(connection,
                                 sql.c_str(),
                                 static_cast<int>(values.size()),
                                 nullptr,
                                 values.empty() ? nullptr : values.data(),
                                 nullptr,
                                 nullptr,
                                 0)};
    if (!result) {
        std::cerr << "[stat-ingest] SQL returned no result: " << PQerrorMessage(connection) << std::endl;
    } else {
        const auto status = PQresultStatus(result.get());
        if (status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK) {
            std::cerr << "[stat-ingest] SQL failed: " << PQresultErrorMessage(result.get())
                      << " statement=" << sql << std::endl;
        }
    }
    return result;
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

bool lockStatWindow(PGconn *connection, int season, int week) {
    return tuplesOk(execute(connection,
        "SELECT pg_advisory_xact_lock(hashtextextended($1, 0))",
        {"stat-ingest:" + std::to_string(season) + ":" + std::to_string(week)}));
}

bool ensureState(PGconn *connection, int season, int week) {
    return commandOk(execute(connection,
        "INSERT INTO stat_ingestion_states "
        "(season, week, version, source_revision, status, stale_after_seconds, updated_at) "
        "VALUES ($1::int, $2::int, 0, 0, 'idle', $3::int, NOW()) "
        "ON CONFLICT (season, week) DO NOTHING",
        {std::to_string(season), std::to_string(week), std::to_string(kDefaultStaleAfterSeconds)}));
}

struct StatStateRecord {
    int season{0};
    int week{1};
    long long version{0};
    long long sourceRevision{0};
    std::string status{"idle"};
    long long activeRunId{0};
    std::string lastSuccessAt;
    long long ageSeconds{-1};
    int staleAfterSeconds{kDefaultStaleAfterSeconds};
    std::string nextRetryAt;
    int providerStatus{0};
    std::string lastError;
    int inserted{0};
    int corrected{0};
    int unchanged{0};
};

StatStateRecord stateRecord(PGconn *connection, int season, int week) {
    StatStateRecord state;
    state.season = season;
    state.week = week;
    auto result = execute(connection,
        "SELECT version, source_revision, status, COALESCE(active_run_id, 0), "
        "COALESCE(to_char(last_success_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), ''), "
        "CASE WHEN last_success_at IS NULL THEN -1 ELSE EXTRACT(EPOCH FROM (NOW() - last_success_at))::bigint END, "
        "stale_after_seconds, "
        "COALESCE(to_char(next_retry_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), ''), "
        "COALESCE(provider_status, 0), last_error, inserted_count, corrected_count, unchanged_count "
        "FROM stat_ingestion_states WHERE season = $1::int AND week = $2::int LIMIT 1",
        {std::to_string(season), std::to_string(week)});
    if (!tuplesOk(result) || PQntuples(result.get()) == 0) return state;
    state.version = cellInt64(result.get(), 0, 0);
    state.sourceRevision = cellInt64(result.get(), 0, 1);
    state.status = cell(result.get(), 0, 2);
    state.activeRunId = cellInt64(result.get(), 0, 3);
    state.lastSuccessAt = cell(result.get(), 0, 4);
    state.ageSeconds = cellInt64(result.get(), 0, 5, -1);
    state.staleAfterSeconds = cellInt(result.get(), 0, 6, kDefaultStaleAfterSeconds);
    state.nextRetryAt = cell(result.get(), 0, 7);
    state.providerStatus = cellInt(result.get(), 0, 8);
    state.lastError = cell(result.get(), 0, 9);
    state.inserted = cellInt(result.get(), 0, 10);
    state.corrected = cellInt(result.get(), 0, 11);
    state.unchanged = cellInt(result.get(), 0, 12);
    return state;
}

struct RunRecord {
    long long id{0};
    std::string runKey;
    std::string ownerId;
    std::string status;
    int attempt{1};
    std::string heartbeatAt;
    std::string leaseExpiresAt;
    bool leaseActive{false};
    int providerStatus{0};
    std::string nextRetryAt;
    long long sourceRevision{0};
};

RunRecord runRecord(PGconn *connection, long long runId) {
    RunRecord run;
    auto result = execute(connection,
        "SELECT id, COALESCE(run_key, ''), owner_id, COALESCE(status, ''), attempt, "
        "COALESCE(to_char(heartbeat_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), ''), "
        "COALESCE(to_char(lease_expires_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), ''), "
        "COALESCE(lease_expires_at > NOW(), FALSE), COALESCE(provider_status, 0), "
        "COALESCE(to_char(next_retry_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), ''), "
        "source_revision "
        "FROM ingestion_runs WHERE id = $1::bigint LIMIT 1",
        {std::to_string(runId)});
    if (!tuplesOk(result) || PQntuples(result.get()) == 0) return run;
    run.id = cellInt64(result.get(), 0, 0);
    run.runKey = cell(result.get(), 0, 1);
    run.ownerId = cell(result.get(), 0, 2);
    run.status = cell(result.get(), 0, 3);
    run.attempt = cellInt(result.get(), 0, 4, 1);
    run.heartbeatAt = cell(result.get(), 0, 5);
    run.leaseExpiresAt = cell(result.get(), 0, 6);
    run.leaseActive = cell(result.get(), 0, 7) == "t";
    run.providerStatus = cellInt(result.get(), 0, 8);
    run.nextRetryAt = cell(result.get(), 0, 9);
    run.sourceRevision = cellInt64(result.get(), 0, 10);
    return run;
}

bool retryWindowActive(PGconn *connection, int season, int week) {
    auto result = execute(connection,
        "SELECT COALESCE(next_retry_at > NOW(), FALSE) "
        "FROM stat_ingestion_states WHERE season = $1::int AND week = $2::int",
        {std::to_string(season), std::to_string(week)});
    return tuplesOk(result) && PQntuples(result.get()) > 0 && cell(result.get(), 0, 0) == "t";
}

bool abandonExpiredRun(PGconn *connection, const StatStateRecord &state) {
    if (state.activeRunId <= 0) return true;
    auto run = runRecord(connection, state.activeRunId);
    if (run.id <= 0 || run.status != "running" || run.leaseActive) return true;
    if (!commandOk(execute(connection,
        "UPDATE ingestion_runs SET status = 'abandoned', finished_at = NOW(), "
        "error_message = CASE WHEN COALESCE(error_message, '') = '' THEN 'Lease expired' ELSE error_message END "
        "WHERE id = $1::bigint AND status = 'running'",
        {std::to_string(run.id)}))) return false;
    return commandOk(execute(connection,
        "UPDATE stat_ingestion_states SET active_run_id = NULL, status = 'idle', "
        "version = version + 1, updated_at = NOW() "
        "WHERE season = $1::int AND week = $2::int AND active_run_id = $3::bigint",
        {std::to_string(state.season), std::to_string(state.week), std::to_string(run.id)}));
}

std::optional<Json::Value> operationReplay(PGconn *connection,
                                           int season,
                                           int week,
                                           const std::string &actor,
                                           const std::string &key,
                                           const std::string &type) {
    if (key.empty()) return std::nullopt;
    auto result = execute(connection,
        "SELECT operation_type, response_payload::text FROM stat_ingestion_operations "
        "WHERE season = $1::int AND week = $2::int AND actor_id = $3 AND operation_key = $4 LIMIT 1",
        {std::to_string(season), std::to_string(week), actor, key});
    if (!tuplesOk(result) || PQntuples(result.get()) == 0) return std::nullopt;
    auto payload = jsonFromString(cell(result.get(), 0, 1));
    payload["idempotentReplay"] = true;
    payload["operationTypeMatches"] = cell(result.get(), 0, 0) == type;
    payload["storedOperationType"] = cell(result.get(), 0, 0);
    return payload;
}

bool storeOperation(PGconn *connection,
                    int season,
                    int week,
                    const std::string &actor,
                    const std::string &key,
                    const std::string &type,
                    long long version,
                    const Json::Value &payload) {
    if (key.empty()) return true;
    return commandOk(execute(connection,
        "INSERT INTO stat_ingestion_operations "
        "(season, week, actor_id, operation_key, operation_type, resulting_version, response_payload) "
        "VALUES ($1::int, $2::int, $3, $4, $5, $6::bigint, $7::jsonb) "
        "ON CONFLICT (season, week, actor_id, operation_key) DO NOTHING",
        {std::to_string(season), std::to_string(week), actor, key, type,
         std::to_string(version), jsonToString(payload)}));
}

Json::Value queuePayload(PGconn *connection, int season, int week) {
    auto result = execute(connection,
        "SELECT league_id, source_revision, status, reason, COALESCE(array_to_json(player_ids)::text, '[]'), "
        "COALESCE(to_char(detected_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), '') "
        "FROM scoring_recalculation_queue WHERE season = $1::int AND week = $2::int "
        "ORDER BY status, league_id",
        {std::to_string(season), std::to_string(week)});
    Json::Value queue(Json::arrayValue);
    if (!tuplesOk(result)) return queue;
    for (int row = 0; row < PQntuples(result.get()); ++row) {
        Json::Value item(Json::objectValue);
        item["leagueId"] = cell(result.get(), row, 0);
        item["sourceRevision"] = Json::Int64(cellInt64(result.get(), row, 1));
        item["status"] = cell(result.get(), row, 2);
        item["reason"] = cell(result.get(), row, 3);
        item["playerIds"] = jsonFromString(cell(result.get(), row, 4), Json::Value{Json::arrayValue});
        item["detectedAt"] = cell(result.get(), row, 5);
        queue.append(item);
    }
    return queue;
}
