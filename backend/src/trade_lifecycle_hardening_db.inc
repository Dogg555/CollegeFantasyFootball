bool lockTradeLeague(PGconn *connection, const std::string &leagueId) {
    if (!tuplesOk(execute(connection,
                          "SELECT pg_advisory_xact_lock(hashtextextended($1, 0))",
                          {"trade:" + leagueId}))) {
        return false;
    }
    return lockRosterLeague(connection, leagueId);
}

struct TradeAccess {
    bool exists{false};
    bool member{false};
    bool commissioner{false};
    Json::Value rosterRules{Json::objectValue};
    Json::Value tradeRules{Json::objectValue};
};

TradeAccess tradeAccess(PGconn *connection,
                        const std::string &leagueId,
                        const std::string &email) {
    TradeAccess access;
    auto result = execute(connection,
        "SELECT lower(l.account_email), l.roster_rules::text, l.trade_rules::text, "
        "COALESCE(lm.status, ''), COALESCE(lm.role, '') "
        "FROM leagues l LEFT JOIN league_members lm "
        "ON lm.league_id = l.id AND lower(lm.email) = lower($2) "
        "WHERE l.id = $1 LIMIT 1",
        {leagueId, email});
    if (!tuplesOk(result) || PQntuples(result.get()) == 0) return access;
    access.exists = true;
    const auto owner = canonicalEmail(cell(result.get(), 0, 0));
    const auto status = lower(cell(result.get(), 0, 3));
    const auto role = lower(cell(result.get(), 0, 4));
    access.member = owner == email || status == "active";
    access.commissioner = owner == email || (status == "active" && role == "commissioner");
    access.rosterRules = jsonFromString(cell(result.get(), 0, 1));
    access.tradeRules = jsonFromString(cell(result.get(), 0, 2));
    return access;
}

bool activeTradeMember(PGconn *connection,
                       const std::string &leagueId,
                       const std::string &email) {
    auto result = execute(connection,
        "SELECT 1 FROM leagues l LEFT JOIN league_members lm "
        "ON lm.league_id = l.id AND lower(lm.email) = lower($2) "
        "WHERE l.id = $1 AND (lower(l.account_email) = lower($2) OR lm.status = 'active') LIMIT 1",
        {leagueId, email});
    return tuplesOk(result) && PQntuples(result.get()) > 0;
}

bool ensureTradeState(PGconn *connection, const std::string &leagueId) {
    return commandOk(execute(connection,
        "INSERT INTO trade_states (league_id, version, updated_at) "
        "VALUES ($1, 0, NOW()) ON CONFLICT (league_id) DO NOTHING",
        {leagueId}));
}

long long tradeVersion(PGconn *connection, const std::string &leagueId) {
    auto result = execute(connection,
        "SELECT version FROM trade_states WHERE league_id = $1 LIMIT 1",
        {leagueId});
    return tuplesOk(result) && PQntuples(result.get()) > 0
        ? cellInt64(result.get(), 0, 0, 0)
        : 0;
}

long long advanceTradeVersion(PGconn *connection, const std::string &leagueId) {
    auto result = execute(connection,
        "INSERT INTO trade_states (league_id, version, updated_at) "
        "VALUES ($1, 1, NOW()) "
        "ON CONFLICT (league_id) DO UPDATE "
        "SET version = trade_states.version + 1, updated_at = NOW() RETURNING version",
        {leagueId});
    return tuplesOk(result) && PQntuples(result.get()) > 0
        ? cellInt64(result.get(), 0, 0, -1)
        : -1;
}

int expireOpenTrades(PGconn *connection, const std::string &leagueId) {
    auto result = execute(connection,
        "WITH expired AS ("
        "UPDATE trade_offers SET status = 'expired', resolution_reason = 'deadline_passed', "
        "resolved_at = COALESCE(resolved_at, NOW()), updated_at = NOW() "
        "WHERE league_id = $1 AND status IN ('pending', 'accepted') "
        "AND expires_at IS NOT NULL AND expires_at <= NOW() RETURNING id), "
        "unlocked AS (DELETE FROM trade_player_locks l USING expired e "
        "WHERE l.offer_id = e.id RETURNING l.offer_id) "
        "SELECT COUNT(*) FROM expired",
        {leagueId});
    if (!tuplesOk(result) || PQntuples(result.get()) == 0) return -1;
    return std::stoi(cell(result.get(), 0, 0));
}

std::string tradeStatusForUi(const std::string &status) {
    if (status == "accepted") return "Accepted";
    if (status == "approved") return "Approved";
    if (status == "declined") return "Declined";
    if (status == "vetoed") return "Vetoed";
    if (status == "cancelled") return "Cancelled";
    if (status == "expired") return "Expired";
    return "Pending";
}

Json::Value tradeOfferFromRow(PGresult *result, int row) {
    Json::Value offer(Json::objectValue);
    offer["id"] = cell(result, row, 0);
    offer["offeredByEmail"] = canonicalEmail(cell(result, row, 1));
    offer["offeredToEmail"] = canonicalEmail(cell(result, row, 2));
    auto offeredPlayer = jsonFromString(cell(result, row, 4));
    if (!offeredPlayer.isObject()) offeredPlayer = Json::Value{Json::objectValue};
    offeredPlayer["id"] = cell(result, row, 3);
    offeredPlayer["playerId"] = cell(result, row, 3);
    auto requestedPlayer = jsonFromString(cell(result, row, 6));
    if (!requestedPlayer.isObject()) requestedPlayer = Json::Value{Json::objectValue};
    requestedPlayer["id"] = cell(result, row, 5);
    requestedPlayer["playerId"] = cell(result, row, 5);
    offer["offerPlayer"] = offeredPlayer;
    offer["requestPlayer"] = requestedPlayer;
    offer["requestPlayerName"] = cell(result, row, 7);
    offer["targetManager"] = canonicalEmail(cell(result, row, 8));
    offer["note"] = cell(result, row, 9);
    offer["requiresApproval"] = cell(result, row, 10) == "t";
    offer["status"] = tradeStatusForUi(cell(result, row, 11));
    offer["createdAt"] = cell(result, row, 12);
    offer["expiresAt"] = cell(result, row, 13);
    offer["resolvedAt"] = cell(result, row, 14);
    offer["acceptedAt"] = cell(result, row, 15);
    offer["approvedAt"] = cell(result, row, 16);
    offer["stateVersion"] = Json::Int64(cellInt64(result, row, 17, 0));
    offer["resolutionReason"] = cell(result, row, 18);
    return offer;
}

Json::Value tradeOffersPayload(PGconn *connection,
                               const std::string &leagueId,
                               const std::string &email,
                               bool commissioner) {
    auto result = execute(connection,
        "SELECT id, lower(offered_by_email), lower(offered_to_email), "
        "COALESCE(offered_player_ids[1], ''), offer_player_snapshot::text, "
        "COALESCE(requested_player_ids[1], ''), request_player_snapshot::text, "
        "request_player_name, lower(target_manager), note, requires_approval, status, "
        "COALESCE(to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), ''), "
        "COALESCE(to_char(expires_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), ''), "
        "COALESCE(to_char(resolved_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), ''), "
        "COALESCE(to_char(accepted_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), ''), "
        "COALESCE(to_char(approved_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), ''), "
        "state_version, resolution_reason "
        "FROM trade_offers WHERE league_id = $1 AND "
        "($3::boolean OR lower(offered_by_email) = lower($2) OR lower(offered_to_email) = lower($2)) "
        "ORDER BY CASE WHEN status IN ('pending', 'accepted') THEN 0 ELSE 1 END, created_at DESC, id DESC",
        {leagueId, email, commissioner ? "true" : "false"});
    Json::Value offers(Json::arrayValue);
    if (!tuplesOk(result)) return offers;
    for (int row = 0; row < PQntuples(result.get()); ++row) {
        offers.append(tradeOfferFromRow(result.get(), row));
    }
    return offers;
}

std::optional<Json::Value> tradeOperationReplay(PGconn *connection,
                                                const std::string &leagueId,
                                                const std::string &email,
                                                const std::string &key,
                                                const std::string &operationType) {
    if (key.empty()) return std::nullopt;
    auto result = execute(connection,
        "SELECT operation_type, response_payload::text FROM trade_operations "
        "WHERE league_id = $1 AND lower(actor_email) = lower($2) AND operation_key = $3 LIMIT 1",
        {leagueId, email, key});
    if (!tuplesOk(result) || PQntuples(result.get()) == 0) return std::nullopt;
    auto payload = jsonFromString(cell(result.get(), 0, 1));
    payload["storedOperationType"] = cell(result.get(), 0, 0);
    payload["operationTypeMatches"] = cell(result.get(), 0, 0) == operationType;
    payload["idempotentReplay"] = true;
    return payload;
}

bool recordTradeOperation(PGconn *connection,
                          const std::string &leagueId,
                          const std::string &email,
                          const std::string &key,
                          const std::string &type,
                          long long version,
                          const Json::Value &payload) {
    if (key.empty()) return true;
    return commandOk(execute(connection,
        "INSERT INTO trade_operations "
        "(league_id, actor_email, operation_key, operation_type, resulting_version, response_payload, created_at) "
        "VALUES ($1, $2, $3, $4, $5::bigint, $6::jsonb, NOW()) "
        "ON CONFLICT (league_id, actor_email, operation_key) DO NOTHING",
        {leagueId, email, key, type, std::to_string(version), jsonToString(payload)}));
}

bool addTradeTransactionRecord(PGconn *connection,
                               const std::string &leagueId,
                               const std::string &email,
                               const std::string &operationKeyValue,
                               const std::string &type,
                               const std::string &summary,
                               const Json::Value &metadata) {
    const auto suffix = operationKeyValue.empty()
        ? std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
        : operationKeyValue;
    const auto id = "trade-" + leagueId + "-" + email + "-" + suffix;
    return commandOk(execute(connection,
        "INSERT INTO transactions (id, league_id, manager_email, transaction_type, summary, metadata, created_at) "
        "VALUES ($1, $2, $3, $4, $5, $6::jsonb, NOW()) ON CONFLICT (id) DO NOTHING",
        {id, leagueId, email, type, summary, jsonToString(metadata)}));
}

std::optional<std::string> lockedTradeOffer(PGconn *connection,
                                            const std::string &leagueId,
                                            const std::string &playerId) {
    auto result = execute(connection,
        "SELECT offer_id FROM trade_player_locks WHERE league_id = $1 AND player_id = $2 LIMIT 1",
        {leagueId, playerId});
    if (!tuplesOk(result) || PQntuples(result.get()) == 0) return std::nullopt;
    return cell(result.get(), 0, 0);
}

bool acquireTradePlayerLocks(PGconn *connection,
                             const std::string &leagueId,
                             const std::string &offerId,
                             const std::string &offeredBy,
                             const std::string &offeredPlayerId,
                             const std::string &offeredTo,
                             const std::string &requestedPlayerId) {
    auto offered = execute(connection,
        "INSERT INTO trade_player_locks "
        "(league_id, player_id, offer_id, manager_email, lock_role, created_at) "
        "VALUES ($1, $2, $3, $4, 'offered', NOW())",
        {leagueId, offeredPlayerId, offerId, offeredBy});
    if (!commandOk(offered)) return false;
    auto requested = execute(connection,
        "INSERT INTO trade_player_locks "
        "(league_id, player_id, offer_id, manager_email, lock_role, created_at) "
        "VALUES ($1, $2, $3, $4, 'requested', NOW())",
        {leagueId, requestedPlayerId, offerId, offeredTo});
    return commandOk(requested);
}

bool releaseTradePlayerLocks(PGconn *connection,
                             const std::string &leagueId,
                             const std::string &offerId) {
    return commandOk(execute(connection,
        "DELETE FROM trade_player_locks WHERE league_id = $1 AND offer_id = $2",
        {leagueId, offerId}));
}

struct TradeRecord {
    bool found{false};
    std::string id;
    std::string offeredBy;
    std::string offeredTo;
    std::string offeredPlayerId;
    std::string requestedPlayerId;
    Json::Value offeredPlayer{Json::objectValue};
    Json::Value requestedPlayer{Json::objectValue};
    std::string status;
    bool requiresApproval{false};
    std::string expiresAt;
};

TradeRecord tradeForUpdate(PGconn *connection,
                           const std::string &leagueId,
                           const std::string &tradeId) {
    TradeRecord trade;
    auto result = execute(connection,
        "SELECT id, lower(offered_by_email), lower(offered_to_email), "
        "COALESCE(offered_player_ids[1], ''), COALESCE(requested_player_ids[1], ''), "
        "offer_player_snapshot::text, request_player_snapshot::text, status, requires_approval, "
        "COALESCE(to_char(expires_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), '') "
        "FROM trade_offers WHERE league_id = $1 AND id = $2 LIMIT 1 FOR UPDATE",
        {leagueId, tradeId});
    if (!tuplesOk(result) || PQntuples(result.get()) == 0) return trade;
    trade.found = true;
    trade.id = cell(result.get(), 0, 0);
    trade.offeredBy = canonicalEmail(cell(result.get(), 0, 1));
    trade.offeredTo = canonicalEmail(cell(result.get(), 0, 2));
    trade.offeredPlayerId = cell(result.get(), 0, 3);
    trade.requestedPlayerId = cell(result.get(), 0, 4);
    trade.offeredPlayer = jsonFromString(cell(result.get(), 0, 5));
    trade.requestedPlayer = jsonFromString(cell(result.get(), 0, 6));
    trade.status = lower(cell(result.get(), 0, 7));
    trade.requiresApproval = cell(result.get(), 0, 8) == "t";
    trade.expiresAt = cell(result.get(), 0, 9);
    return trade;
}

struct TradeContext {
    PgConnection connection;
    TradeAccess access;
    long long version{0};
    int expiredCount{0};
};

std::optional<TradeContext> openTradeContext(const std::string &leagueId,
                                             const std::string &email) {
    TradeContext context;
    context.connection = connectDb();
    if (!context.connection || !begin(context.connection.get())
        || !lockTradeLeague(context.connection.get(), leagueId)) {
        return std::nullopt;
    }
    context.access = tradeAccess(context.connection.get(), leagueId, email);
    if (!context.access.exists) return context;
    if (!ensureTradeState(context.connection.get(), leagueId)) {
        rollback(context.connection.get());
        return std::nullopt;
    }
    context.expiredCount = expireOpenTrades(context.connection.get(), leagueId);
    if (context.expiredCount < 0) {
        rollback(context.connection.get());
        return std::nullopt;
    }
    if (context.expiredCount > 0 && advanceTradeVersion(context.connection.get(), leagueId) < 0) {
        rollback(context.connection.get());
        return std::nullopt;
    }
    context.version = tradeVersion(context.connection.get(), leagueId);
    return context;
}
