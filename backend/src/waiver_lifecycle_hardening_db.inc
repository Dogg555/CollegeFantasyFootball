bool lockWaiverLeague(PGconn *connection, const std::string &leagueId) {
    if (!tuplesOk(execute(connection,
                          "SELECT pg_advisory_xact_lock(hashtextextended($1, 0))",
                          {"waiver:" + leagueId}))) {
        return false;
    }
    return lockRosterLeague(connection, leagueId);
}

bool isCommissioner(PGconn *connection,
                    const std::string &leagueId,
                    const std::string &email) {
    auto result = execute(connection,
        "SELECT 1 FROM leagues l LEFT JOIN league_members lm "
        "ON lm.league_id = l.id AND lower(lm.email) = lower($2) "
        "WHERE l.id = $1 AND (lower(l.account_email) = lower($2) "
        "OR (lm.role = 'commissioner' AND lm.status = 'active')) LIMIT 1",
        {leagueId, email});
    return tuplesOk(result) && PQntuples(result.get()) > 0;
}

bool ensureWaiverState(PGconn *connection, const std::string &leagueId) {
    return commandOk(execute(connection,
        "INSERT INTO waiver_states (league_id, version, updated_at) "
        "VALUES ($1, 0, NOW()) ON CONFLICT (league_id) DO NOTHING",
        {leagueId}));
}

long long waiverVersion(PGconn *connection, const std::string &leagueId) {
    auto result = execute(connection,
        "SELECT version FROM waiver_states WHERE league_id = $1 LIMIT 1",
        {leagueId});
    return tuplesOk(result) && PQntuples(result.get()) > 0
        ? cellInt64(result.get(), 0, 0, 0)
        : 0;
}

long long advanceWaiverVersion(PGconn *connection,
                               const std::string &leagueId,
                               const std::string &processingRunId = "") {
    auto result = execute(connection,
        "INSERT INTO waiver_states "
        "(league_id, version, last_processed_at, last_processing_run_id, updated_at) "
        "VALUES ($1, 1, CASE WHEN $2 = '' THEN NULL ELSE NOW() END, NULLIF($2, ''), NOW()) "
        "ON CONFLICT (league_id) DO UPDATE SET "
        "version = waiver_states.version + 1, "
        "last_processed_at = CASE WHEN $2 = '' THEN waiver_states.last_processed_at ELSE NOW() END, "
        "last_processing_run_id = CASE WHEN $2 = '' THEN waiver_states.last_processing_run_id ELSE $2 END, "
        "updated_at = NOW() RETURNING version",
        {leagueId, processingRunId});
    return tuplesOk(result) && PQntuples(result.get()) > 0
        ? cellInt64(result.get(), 0, 0, -1)
        : -1;
}

bool seedWaiverPriorities(PGconn *connection, const std::string &leagueId) {
    auto inserted = execute(connection,
        "INSERT INTO waiver_priorities (league_id, manager_email, priority, updated_at) "
        "SELECT league_id, lower(email), "
        "(ROW_NUMBER() OVER (ORDER BY CASE WHEN role = 'commissioner' THEN 0 ELSE 1 END, "
        "COALESCE(joined_at, created_at), lower(email)))::int, NOW() "
        "FROM league_members WHERE league_id = $1 AND status = 'active' "
        "ON CONFLICT (league_id, manager_email) DO NOTHING",
        {leagueId});
    if (!commandOk(inserted)) return false;
    auto dense = execute(connection,
        "WITH ranked AS ("
        "SELECT league_id, manager_email, "
        "ROW_NUMBER() OVER (ORDER BY priority, lower(manager_email))::int AS next_priority "
        "FROM waiver_priorities WHERE league_id = $1) "
        "UPDATE waiver_priorities w SET priority = ranked.next_priority, updated_at = NOW() "
        "FROM ranked WHERE w.league_id = ranked.league_id AND w.manager_email = ranked.manager_email",
        {leagueId});
    return commandOk(dense);
}

Json::Value waiverPriorityPayload(PGconn *connection, const std::string &leagueId) {
    (void)seedWaiverPriorities(connection, leagueId);
    auto result = execute(connection,
        "SELECT lower(m.email), m.role, m.status, COALESCE(w.priority, 9999) "
        "FROM league_members m LEFT JOIN waiver_priorities w "
        "ON w.league_id = m.league_id AND lower(w.manager_email) = lower(m.email) "
        "WHERE m.league_id = $1 AND m.status = 'active' "
        "ORDER BY COALESCE(w.priority, 9999), lower(m.email)",
        {leagueId});
    Json::Value priorities(Json::arrayValue);
    if (!tuplesOk(result)) return priorities;
    for (int row = 0; row < PQntuples(result.get()); ++row) {
        Json::Value item(Json::objectValue);
        item["managerEmail"] = canonicalEmail(cell(result.get(), row, 0));
        item["role"] = cell(result.get(), row, 1);
        item["status"] = cell(result.get(), row, 2) == "active" ? "Active" : cell(result.get(), row, 2);
        item["priority"] = std::stoi(cell(result.get(), row, 3));
        priorities.append(item);
    }
    return priorities;
}

std::string waiverStatusForUi(const std::string &status) {
    if (status == "processed") return "Processed";
    if (status == "cancelled") return "Cancelled";
    if (status == "failed") return "Failed";
    return "Pending";
}

Json::Value waiverClaimsPayload(PGconn *connection,
                                const std::string &leagueId,
                                const std::string &email,
                                bool commissioner) {
    auto result = execute(connection,
        "SELECT c.id, c.add_player_id, c.add_player_snapshot::text, "
        "COALESCE(c.drop_player_id, ''), c.status, "
        "COALESCE(to_char(c.created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), ''), "
        "lower(c.manager_email), COALESCE(w.priority, c.priority, 9999), c.claim_order, "
        "COALESCE(c.failure_code, ''), "
        "COALESCE(to_char(c.processed_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), ''), "
        "COALESCE(c.resolved_by_email, ''), COALESCE(c.resolution_run_id, '') "
        "FROM waiver_claims c LEFT JOIN waiver_priorities w "
        "ON w.league_id = c.league_id AND lower(w.manager_email) = lower(c.manager_email) "
        "WHERE c.league_id = $1 AND ($3::boolean OR lower(c.manager_email) = lower($2)) "
        "ORDER BY CASE WHEN c.status = 'pending' THEN 0 ELSE 1 END, "
        "COALESCE(w.priority, c.priority, 9999), c.claim_order, c.created_at, c.id",
        {leagueId, email, commissioner ? "true" : "false"});
    Json::Value claims(Json::arrayValue);
    if (!tuplesOk(result)) return claims;
    for (int row = 0; row < PQntuples(result.get()); ++row) {
        Json::Value claim(Json::objectValue);
        claim["id"] = cell(result.get(), row, 0);
        auto player = jsonFromString(cell(result.get(), row, 2));
        if (!player.isObject()) player = Json::Value{Json::objectValue};
        player["id"] = cell(result.get(), row, 1);
        player["playerId"] = cell(result.get(), row, 1);
        claim["addPlayer"] = player;
        claim["dropPlayerId"] = cell(result.get(), row, 3);
        claim["status"] = waiverStatusForUi(cell(result.get(), row, 4));
        claim["createdAt"] = cell(result.get(), row, 5);
        claim["managerEmail"] = canonicalEmail(cell(result.get(), row, 6));
        claim["priority"] = std::stoi(cell(result.get(), row, 7));
        claim["claimOrder"] = std::stoi(cell(result.get(), row, 8));
        claim["failureCode"] = cell(result.get(), row, 9);
        claim["processedAt"] = cell(result.get(), row, 10);
        claim["resolvedByEmail"] = canonicalEmail(cell(result.get(), row, 11));
        claim["resolutionRunId"] = cell(result.get(), row, 12);
        claims.append(claim);
    }
    return claims;
}

std::optional<Json::Value> waiverOperationReplay(PGconn *connection,
                                                 const std::string &leagueId,
                                                 const std::string &key,
                                                 const std::string &operationType) {
    if (key.empty()) return std::nullopt;
    auto result = execute(connection,
        "SELECT operation_type, response_payload::text FROM waiver_operations "
        "WHERE league_id = $1 AND operation_key = $2 LIMIT 1",
        {leagueId, key});
    if (!tuplesOk(result) || PQntuples(result.get()) == 0) return std::nullopt;
    auto payload = jsonFromString(cell(result.get(), 0, 1));
    payload["storedOperationType"] = cell(result.get(), 0, 0);
    payload["operationTypeMatches"] = cell(result.get(), 0, 0) == operationType;
    payload["idempotentReplay"] = true;
    return payload;
}

bool recordWaiverOperation(PGconn *connection,
                           const std::string &leagueId,
                           const std::string &key,
                           const std::string &type,
                           long long version,
                           const Json::Value &payload) {
    if (key.empty()) return true;
    return commandOk(execute(connection,
        "INSERT INTO waiver_operations "
        "(league_id, operation_key, operation_type, resulting_version, response_payload, created_at) "
        "VALUES ($1, $2, $3, $4::bigint, $5::jsonb, NOW()) "
        "ON CONFLICT (league_id, operation_key) DO NOTHING",
        {leagueId, key, type, std::to_string(version), jsonToString(payload)}));
}

bool addWaiverTransactionRecord(PGconn *connection,
                                const std::string &leagueId,
                                const std::string &email,
                                const std::string &operationKeyValue,
                                const std::string &type,
                                const std::string &summary,
                                const Json::Value &metadata) {
    const auto suffix = operationKeyValue.empty()
        ? std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
        : operationKeyValue;
    const auto id = "waiver-" + leagueId + "-" + suffix;
    return commandOk(execute(connection,
        "INSERT INTO transactions (id, league_id, manager_email, transaction_type, summary, metadata, created_at) "
        "VALUES ($1, $2, $3, $4, $5, $6::jsonb, NOW()) ON CONFLICT (id) DO NOTHING",
        {id, leagueId, email, type, summary, jsonToString(metadata)}));
}

bool moveManagerToBackDb(PGconn *connection,
                         const std::string &leagueId,
                         const std::string &managerEmail) {
    auto move = execute(connection,
        "UPDATE waiver_priorities SET priority = "
        "COALESCE((SELECT MAX(priority) + 1 FROM waiver_priorities WHERE league_id = $1), 1), "
        "updated_at = NOW() WHERE league_id = $1 AND lower(manager_email) = lower($2)",
        {leagueId, managerEmail});
    if (!commandOk(move)) return false;
    auto dense = execute(connection,
        "WITH ranked AS ("
        "SELECT league_id, manager_email, "
        "ROW_NUMBER() OVER (ORDER BY priority, lower(manager_email))::int AS next_priority "
        "FROM waiver_priorities WHERE league_id = $1) "
        "UPDATE waiver_priorities w SET priority = ranked.next_priority, updated_at = NOW() "
        "FROM ranked WHERE w.league_id = ranked.league_id AND w.manager_email = ranked.manager_email",
        {leagueId});
    return commandOk(dense);
}

int nextManagerClaimOrder(PGconn *connection,
                          const std::string &leagueId,
                          const std::string &managerEmail) {
    auto result = execute(connection,
        "SELECT COALESCE(MAX(claim_order) + 1, 1) FROM waiver_claims "
        "WHERE league_id = $1 AND lower(manager_email) = lower($2) AND status = 'pending'",
        {leagueId, managerEmail});
    if (!tuplesOk(result) || PQntuples(result.get()) == 0) return 1;
    return std::stoi(cell(result.get(), 0, 0));
}

int pendingManagerClaimCount(PGconn *connection,
                             const std::string &leagueId,
                             const std::string &managerEmail) {
    auto result = execute(connection,
        "SELECT COUNT(*) FROM waiver_claims "
        "WHERE league_id = $1 AND lower(manager_email) = lower($2) AND status = 'pending'",
        {leagueId, managerEmail});
    if (!tuplesOk(result) || PQntuples(result.get()) == 0) return 0;
    return std::stoi(cell(result.get(), 0, 0));
}

Json::Value pendingManagerClaims(PGconn *connection,
                                 const std::string &leagueId,
                                 const std::string &managerEmail) {
    auto result = execute(connection,
        "SELECT id, lower(manager_email), status, claim_order, "
        "COALESCE(to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), '') "
        "FROM waiver_claims WHERE league_id = $1 AND lower(manager_email) = lower($2) "
        "AND status = 'pending' ORDER BY claim_order, created_at, id",
        {leagueId, managerEmail});
    Json::Value claims(Json::arrayValue);
    if (!tuplesOk(result)) return claims;
    for (int row = 0; row < PQntuples(result.get()); ++row) {
        Json::Value claim(Json::objectValue);
        claim["id"] = cell(result.get(), row, 0);
        claim["managerEmail"] = canonicalEmail(cell(result.get(), row, 1));
        claim["status"] = cell(result.get(), row, 2);
        claim["claimOrder"] = std::stoi(cell(result.get(), row, 3));
        claim["createdAt"] = cell(result.get(), row, 4);
        claims.append(claim);
    }
    return claims;
}
