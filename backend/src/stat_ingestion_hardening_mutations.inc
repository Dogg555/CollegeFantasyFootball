struct StatContext {
    PgConnection connection;
    StatStateRecord state;
};

std::optional<StatContext> openStatContext(int season, int week) {
    auto connection = connectDb();
    if (!connection || !begin(connection.get()) || !lockStatWindow(connection.get(), season, week)
        || !ensureState(connection.get(), season, week)) {
        if (connection) rollback(connection.get());
        return std::nullopt;
    }
    (void)execute(connection.get(), "SELECT cff_mark_stat_source_stale()");
    StatContext context;
    context.connection = std::move(connection);
    context.state = stateRecord(context.connection.get(), season, week);
    return context;
}

bool expectedVersionMatches(const Json::Value &body, long long currentVersion) {
    return body.isObject() && body.isMember("expectedVersion")
        && int64Value(body["expectedVersion"], -1) == currentVersion;
}

std::string requestedOwner(const Json::Value &body, const std::string &actor) {
    const auto owner = trim(body.get("ownerId", "").asString());
    return owner.empty() ? actor : owner;
}

bool runOwnedAndActive(const RunRecord &run, const std::string &owner) {
    return run.id > 0 && run.status == "running" && run.leaseActive && run.ownerId == owner;
}

std::string pgTextArray(const std::set<std::string> &values) {
    std::ostringstream output;
    output << '{';
    bool first = true;
    for (const auto &value : values) {
        if (!first) output << ',';
        first = false;
        output << '"';
        for (const char ch : value) {
            if (ch == '"' || ch == '\\') output << '\\';
            output << ch;
        }
        output << '"';
    }
    output << '}';
    return output.str();
}

drogon::HttpResponsePtr getStatStatus(int season,
                                      int week,
                                      const std::string &actor) {
    auto context = openStatContext(season, week);
    if (!context) return statStorageUnavailable();
    context->state = stateRecord(context->connection.get(), season, week);
    auto payload = statStatePayload(context->connection.get(), context->state, actor);
    if (!commit(context->connection.get())) {
        rollback(context->connection.get());
        return statStorageUnavailable();
    }
    return jsonResponse(payload);
}

drogon::HttpResponsePtr startStatRun(const drogon::HttpRequestPtr &request,
                                     int season,
                                     int week,
                                     const std::string &actor,
                                     const Json::Value &body) {
    const auto key = operationKey(request);
    if (key.empty()) {
        return errorResponse(drogon::k400BadRequest,
                             "An Idempotency-Key is required.",
                             "idempotency_key_required");
    }
    auto context = openStatContext(season, week);
    if (!context) return statStorageUnavailable();
    if (const auto replay = operationReplay(context->connection.get(), season, week, actor, key, "start")) {
        if (!(*replay)["operationTypeMatches"].asBool()) {
            rollback(context->connection.get());
            return errorResponse(drogon::k409Conflict,
                                 "This idempotency key was used for another ingestion action.",
                                 "idempotency_key_conflict");
        }
        auto payload = *replay;
        payload.removeMember("operationTypeMatches");
        payload.removeMember("storedOperationType");
        if (!commit(context->connection.get())) return statStorageUnavailable();
        return jsonResponse(payload);
    }
    if (!expectedVersionMatches(body, context->state.version)) {
        auto details = statConflictDetails(context->connection.get(), context->state, actor);
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict,
                             "Ingestion state changed. Load the latest state before retrying.",
                             "stat_ingestion_state_conflict",
                             false,
                             details);
    }
    if (!abandonExpiredRun(context->connection.get(), context->state)) {
        rollback(context->connection.get());
        return statStorageUnavailable();
    }
    context->state = stateRecord(context->connection.get(), season, week);
    if (retryWindowActive(context->connection.get(), season, week)) {
        auto details = statConflictDetails(context->connection.get(), context->state, actor);
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict,
                             "The provider retry window is still active.",
                             "ingestion_backoff_active",
                             true,
                             details);
    }
    if (context->state.activeRunId > 0) {
        const auto active = runRecord(context->connection.get(), context->state.activeRunId);
        if (active.status == "running" && active.leaseActive) {
            auto details = statConflictDetails(context->connection.get(), context->state, actor);
            rollback(context->connection.get());
            return errorResponse(drogon::k409Conflict,
                                 "Another ingestion worker owns this season and week.",
                                 "ingestion_run_active",
                                 true,
                                 details);
        }
    }

    const auto owner = requestedOwner(body, actor);
    const auto runKey = trim(body.get("runKey", key).asString());
    const auto leaseSeconds = std::clamp(positiveInt(body.get("leaseSeconds", kDefaultLeaseSeconds),
                                                      kDefaultLeaseSeconds), 30, 1800);
    auto attemptResult = execute(context->connection.get(),
        "SELECT COALESCE(MAX(attempt), 0) + 1 FROM ingestion_runs "
        "WHERE resource = 'player_stats' AND season = $1::int AND week = $2::int",
        {std::to_string(season), std::to_string(week)});
    const auto attempt = tuplesOk(attemptResult) && PQntuples(attemptResult.get()) > 0
        ? cellInt(attemptResult.get(), 0, 0, 1) : 1;
    auto inserted = execute(context->connection.get(),
        "INSERT INTO ingestion_runs "
        "(resource, season, week, status, run_key, owner_id, heartbeat_at, lease_expires_at, "
        "attempt, call_count, row_count, metadata, started_at) "
        "VALUES ('player_stats', $1::int, $2::int, 'running', $3, $4, NOW(), "
        "NOW() + make_interval(secs => $5::int), $6::int, 0, 0, $7::jsonb, NOW()) "
        "RETURNING id",
        {std::to_string(season), std::to_string(week), runKey, owner,
         std::to_string(leaseSeconds), std::to_string(attempt),
         jsonToString(body.get("metadata", Json::Value{Json::objectValue}))});
    if (!tuplesOk(inserted) || PQntuples(inserted.get()) == 0) {
        rollback(context->connection.get());
        return statStorageUnavailable();
    }
    const auto runId = cellInt64(inserted.get(), 0, 0);
    auto update = execute(context->connection.get(),
        "UPDATE stat_ingestion_states SET status = 'running', active_run_id = $3::bigint, "
        "version = version + 1, next_retry_at = NULL, last_error = '', updated_at = NOW() "
        "WHERE season = $1::int AND week = $2::int RETURNING version",
        {std::to_string(season), std::to_string(week), std::to_string(runId)});
    if (!tuplesOk(update) || PQntuples(update.get()) == 0) {
        rollback(context->connection.get());
        return statStorageUnavailable();
    }
    context->state = stateRecord(context->connection.get(), season, week);
    auto payload = statStatePayload(context->connection.get(), context->state, actor);
    payload["started"] = true;
    payload["runId"] = Json::Int64(runId);
    payload["ownerId"] = owner;
    payload["attempt"] = attempt;
    if (!storeOperation(context->connection.get(), season, week, actor, key, "start",
                        context->state.version, payload)
        || !commit(context->connection.get())) {
        rollback(context->connection.get());
        return statStorageUnavailable();
    }
    return jsonResponse(payload, drogon::k201Created);
}

drogon::HttpResponsePtr heartbeatStatRun(const drogon::HttpRequestPtr &request,
                                         int season,
                                         int week,
                                         const std::string &actor,
                                         const Json::Value &body) {
    const auto key = operationKey(request);
    if (key.empty()) return errorResponse(drogon::k400BadRequest, "An Idempotency-Key is required.", "idempotency_key_required");
    auto context = openStatContext(season, week);
    if (!context) return statStorageUnavailable();
    if (const auto replay = operationReplay(context->connection.get(), season, week, actor, key, "heartbeat")) {
        auto payload = *replay;
        if (!payload["operationTypeMatches"].asBool()) {
            rollback(context->connection.get());
            return errorResponse(drogon::k409Conflict, "This idempotency key was used for another action.", "idempotency_key_conflict");
        }
        payload.removeMember("operationTypeMatches"); payload.removeMember("storedOperationType");
        if (!commit(context->connection.get())) return statStorageUnavailable();
        return jsonResponse(payload);
    }
    const auto runId = int64Value(body.get("runId", 0), 0);
    const auto owner = requestedOwner(body, actor);
    const auto run = runRecord(context->connection.get(), runId);
    if (!runOwnedAndActive(run, owner)) {
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict, "The ingestion lease is not owned by this worker.", "ingestion_lease_lost");
    }
    const auto leaseSeconds = std::clamp(positiveInt(body.get("leaseSeconds", kDefaultLeaseSeconds),
                                                      kDefaultLeaseSeconds), 30, 1800);
    auto update = execute(context->connection.get(),
        "UPDATE ingestion_runs SET heartbeat_at = NOW(), lease_expires_at = NOW() + make_interval(secs => $3::int) "
        "WHERE id = $1::bigint AND owner_id = $2 AND status = 'running' RETURNING id",
        {std::to_string(runId), owner, std::to_string(leaseSeconds)});
    if (!tuplesOk(update) || PQntuples(update.get()) == 0) {
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict, "The ingestion lease was lost.", "ingestion_lease_lost");
    }
    context->state = stateRecord(context->connection.get(), season, week);
    auto payload = statStatePayload(context->connection.get(), context->state, actor);
    payload["heartbeat"] = true;
    if (!storeOperation(context->connection.get(), season, week, actor, key, "heartbeat",
                        context->state.version, payload)
        || !commit(context->connection.get())) return statStorageUnavailable();
    return jsonResponse(payload);
}

bool enqueueAffectedLeagues(PGconn *connection,
                            int season,
                            int week,
                            long long sourceRevision,
                            const std::set<std::string> &playerIds) {
    std::map<std::string, std::string> leagues;
    for (const auto &playerId : playerIds) {
        auto result = execute(connection,
            "SELECT DISTINCT r.league_id, COALESCE(s.status, 'unscored') "
            "FROM rosters r LEFT JOIN scoring_week_states s "
            "ON s.league_id = r.league_id AND s.season = $2::int AND s.week = $3::int "
            "WHERE r.player_id = $1",
            {playerId, std::to_string(season), std::to_string(week)});
        if (!tuplesOk(result)) return false;
        for (int row = 0; row < PQntuples(result.get()); ++row) {
            const auto leagueId = cell(result.get(), row, 0);
            const auto scoringStatus = cell(result.get(), row, 1);
            if (!leagueId.empty()) leagues[leagueId] = scoringStatus;
        }
    }
    const auto playerArray = pgTextArray(playerIds);
    for (const auto &[leagueId, scoringStatus] : leagues) {
        const auto queueStatus = cff::stat_ingestion_lifecycle::recalculationStatus(scoringStatus);
        if (queueStatus == "not_required") continue;
        const auto reason = cff::stat_ingestion_lifecycle::recalculationReason(scoringStatus);
        if (!commandOk(execute(connection,
            "INSERT INTO scoring_recalculation_queue "
            "(league_id, season, week, source_revision, status, reason, player_ids, detected_at, updated_at) "
            "VALUES ($1, $2::int, $3::int, $4::bigint, $5, $6, $7::text[], NOW(), NOW()) "
            "ON CONFLICT (league_id, season, week) DO UPDATE SET "
            "source_revision = EXCLUDED.source_revision, status = EXCLUDED.status, reason = EXCLUDED.reason, "
            "player_ids = ARRAY(SELECT DISTINCT unnest(scoring_recalculation_queue.player_ids || EXCLUDED.player_ids)), "
            "detected_at = NOW(), processed_at = NULL, updated_at = NOW()",
            {leagueId, std::to_string(season), std::to_string(week), std::to_string(sourceRevision),
             queueStatus, reason, playerArray}))) return false;
    }
    return true;
}

drogon::HttpResponsePtr applyStatRun(const drogon::HttpRequestPtr &request,
                                     int season,
                                     int week,
                                     const std::string &actor,
                                     const Json::Value &body) {
    const auto key = operationKey(request);
    if (key.empty()) return errorResponse(drogon::k400BadRequest, "An Idempotency-Key is required.", "idempotency_key_required");
    auto context = openStatContext(season, week);
    if (!context) return statStorageUnavailable();
    if (const auto replay = operationReplay(context->connection.get(), season, week, actor, key, "apply")) {
        auto payload = *replay;
        if (!payload["operationTypeMatches"].asBool()) {
            rollback(context->connection.get());
            return errorResponse(drogon::k409Conflict, "This idempotency key was used for another action.", "idempotency_key_conflict");
        }
        payload.removeMember("operationTypeMatches"); payload.removeMember("storedOperationType");
        if (!commit(context->connection.get())) return statStorageUnavailable();
        return jsonResponse(payload);
    }
    if (!expectedVersionMatches(body, context->state.version)) {
        auto details = statConflictDetails(context->connection.get(), context->state, actor);
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict, "Ingestion state changed.", "stat_ingestion_state_conflict", false, details);
    }
    const auto runId = int64Value(body.get("runId", 0), 0);
    const auto owner = requestedOwner(body, actor);
    const auto run = runRecord(context->connection.get(), runId);
    if (!runOwnedAndActive(run, owner) || context->state.activeRunId != runId) {
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict, "The ingestion lease is not owned by this worker.", "ingestion_lease_lost");
    }
    const auto records = body.get("records", Json::Value{Json::arrayValue});
    if (!records.isArray() || records.empty()) {
        rollback(context->connection.get());
        return errorResponse(drogon::k400BadRequest, "At least one stat record is required.", "stat_records_required");
    }

    std::map<std::string, Json::Value> uniqueRecords;
    for (const auto &raw : records) {
        Json::Value record = raw;
        record["season"] = season;
        record["week"] = week;
        const auto recordKey = cff::stat_ingestion_lifecycle::statRecordKey(record);
        if (!recordKey.empty()) uniqueRecords[recordKey] = record;
    }
    int insertedCount = 0;
    int correctedCount = 0;
    int unchangedCount = 0;
    std::set<std::string> changedPlayers;
    const auto candidateRevision = context->state.sourceRevision + 1;
    const std::set<std::string> allowedCategories{"passing", "rushing", "receiving", "defense"};

    for (const auto &[_, record] : uniqueRecords) {
        const auto playerId = trim(record.get("playerId", "").asString());
        const auto category = cff::stat_ingestion_lifecycle::canonicalToken(record.get("category", "").asString());
        const auto statName = cff::stat_ingestion_lifecycle::canonicalToken(record.get("statName", "").asString());
        const auto gameId = int64Value(record.get("gameId", 0), 0);
        const auto statValue = numberValue(record.get("statValue", 0.0));
        const auto team = trim(record.get("team", "").asString());
        const auto conference = trim(record.get("conference", "").asString());
        if (playerId.empty() || statName.empty() || gameId <= 0 || allowedCategories.find(category) == allowedCategories.end()) {
            rollback(context->connection.get());
            return errorResponse(drogon::k400BadRequest, "A stat record is invalid.", "invalid_stat_record");
        }
        auto dependencies = execute(context->connection.get(),
            "SELECT EXISTS(SELECT 1 FROM players WHERE id = $1), EXISTS(SELECT 1 FROM games WHERE id = $2::bigint)",
            {playerId, std::to_string(gameId)});
        if (!tuplesOk(dependencies) || PQntuples(dependencies.get()) == 0
            || cell(dependencies.get(), 0, 0) != "t" || cell(dependencies.get(), 0, 1) != "t") {
            rollback(context->connection.get());
            return errorResponse(drogon::k409Conflict, "The stat references an unknown player or game.", "stat_dependency_missing");
        }
        Json::Value normalized = record;
        normalized["playerId"] = playerId;
        normalized["category"] = category;
        normalized["statName"] = statName;
        normalized["gameId"] = Json::Int64(gameId);
        normalized["statValue"] = statValue;
        const auto sourceHash = cff::stat_ingestion_lifecycle::statSourceHash(normalized);
        auto existing = execute(context->connection.get(),
            "SELECT stat_value, source_hash FROM player_stats "
            "WHERE player_id = $1 AND season = $2::int AND week = $3::int AND category = $4 "
            "AND stat_name = $5 AND game_id = $6::bigint LIMIT 1",
            {playerId, std::to_string(season), std::to_string(week), category, statName, std::to_string(gameId)});
        if (!tuplesOk(existing)) {
            rollback(context->connection.get());
            return statStorageUnavailable();
        }
        if (PQntuples(existing.get()) == 0) {
            if (!commandOk(execute(context->connection.get(),
                "INSERT INTO player_stats "
                "(player_id, season, week, team, conference, category, stat_name, stat_value, game_id, "
                "source_hash, source_revision, ingestion_run_id, updated_at) "
                "VALUES ($1, $2::int, $3::int, NULLIF($4, ''), NULLIF($5, ''), $6, $7, $8::numeric, "
                "$9::bigint, $10, $11::bigint, $12::bigint, NOW())",
                {playerId, std::to_string(season), std::to_string(week), team, conference, category, statName,
                 std::to_string(statValue), std::to_string(gameId), sourceHash,
                 std::to_string(candidateRevision), std::to_string(runId)}))) {
                rollback(context->connection.get()); return statStorageUnavailable();
            }
            if (!commandOk(execute(context->connection.get(),
                "INSERT INTO player_stat_revisions "
                "(ingestion_run_id, player_id, season, week, category, stat_name, game_id, change_type, "
                "previous_value, new_value, previous_hash, source_hash, source_revision, raw_payload) "
                "VALUES ($1::bigint, $2, $3::int, $4::int, $5, $6, $7::bigint, 'inserted', NULL, "
                "$8::numeric, '', $9, $10::bigint, $11::jsonb)",
                {std::to_string(runId), playerId, std::to_string(season), std::to_string(week), category,
                 statName, std::to_string(gameId), std::to_string(statValue), sourceHash,
                 std::to_string(candidateRevision), jsonToString(normalized)}))) {
                rollback(context->connection.get()); return statStorageUnavailable();
            }
            ++insertedCount;
            changedPlayers.insert(playerId);
        } else if (cell(existing.get(), 0, 1) == sourceHash) {
            ++unchangedCount;
        } else {
            const auto previousValue = cell(existing.get(), 0, 0);
            const auto previousHash = cell(existing.get(), 0, 1);
            if (!commandOk(execute(context->connection.get(),
                "UPDATE player_stats SET stat_value = $7::numeric, team = NULLIF($8, ''), "
                "conference = NULLIF($9, ''), source_hash = $10, source_revision = $11::bigint, "
                "ingestion_run_id = $12::bigint, corrected_at = NOW(), updated_at = NOW() "
                "WHERE player_id = $1 AND season = $2::int AND week = $3::int AND category = $4 "
                "AND stat_name = $5 AND game_id = $6::bigint",
                {playerId, std::to_string(season), std::to_string(week), category, statName,
                 std::to_string(gameId), std::to_string(statValue), team, conference, sourceHash,
                 std::to_string(candidateRevision), std::to_string(runId)}))) {
                rollback(context->connection.get()); return statStorageUnavailable();
            }
            if (!commandOk(execute(context->connection.get(),
                "INSERT INTO player_stat_revisions "
                "(ingestion_run_id, player_id, season, week, category, stat_name, game_id, change_type, "
                "previous_value, new_value, previous_hash, source_hash, source_revision, raw_payload) "
                "VALUES ($1::bigint, $2, $3::int, $4::int, $5, $6, $7::bigint, 'corrected', "
                "$8::numeric, $9::numeric, $10, $11, $12::bigint, $13::jsonb)",
                {std::to_string(runId), playerId, std::to_string(season), std::to_string(week), category,
                 statName, std::to_string(gameId), previousValue, std::to_string(statValue), previousHash,
                 sourceHash, std::to_string(candidateRevision), jsonToString(normalized)}))) {
                rollback(context->connection.get()); return statStorageUnavailable();
            }
            ++correctedCount;
            changedPlayers.insert(playerId);
        }
    }

    const auto changedCount = insertedCount + correctedCount;
    const auto resultingRevision = changedCount > 0 ? candidateRevision : context->state.sourceRevision;
    if (changedCount > 0 && !enqueueAffectedLeagues(context->connection.get(), season, week,
                                                     resultingRevision, changedPlayers)) {
        rollback(context->connection.get()); return statStorageUnavailable();
    }
    auto runUpdate = execute(context->connection.get(),
        "UPDATE ingestion_runs SET status = 'success', finished_at = NOW(), heartbeat_at = NOW(), "
        "lease_expires_at = NOW(), row_count = $2::int, call_count = $3::int, source_revision = $4::bigint, "
        "provider_status = NULL, next_retry_at = NULL, error_message = NULL, metadata = metadata || $5::jsonb "
        "WHERE id = $1::bigint AND status = 'running' RETURNING id",
        {std::to_string(runId), std::to_string(static_cast<int>(uniqueRecords.size())),
         std::to_string(positiveInt(body.get("apiCalls", 1), 1)), std::to_string(resultingRevision),
         jsonToString(body.get("metadata", Json::Value{Json::objectValue}))});
    if (!tuplesOk(runUpdate) || PQntuples(runUpdate.get()) == 0) {
        rollback(context->connection.get()); return statStorageUnavailable();
    }
    auto stateUpdate = execute(context->connection.get(),
        "UPDATE stat_ingestion_states SET version = version + 1, source_revision = $3::bigint, "
        "status = 'fresh', active_run_id = NULL, last_success_at = NOW(), next_retry_at = NULL, "
        "provider_status = NULL, last_error = '', inserted_count = $4::int, corrected_count = $5::int, "
        "unchanged_count = $6::int, updated_at = NOW() "
        "WHERE season = $1::int AND week = $2::int RETURNING version",
        {std::to_string(season), std::to_string(week), std::to_string(resultingRevision),
         std::to_string(insertedCount), std::to_string(correctedCount), std::to_string(unchangedCount)});
    if (!tuplesOk(stateUpdate) || PQntuples(stateUpdate.get()) == 0) {
        rollback(context->connection.get()); return statStorageUnavailable();
    }
    context->state = stateRecord(context->connection.get(), season, week);
    auto payload = statStatePayload(context->connection.get(), context->state, actor);
    payload["applied"] = true;
    payload["runId"] = Json::Int64(runId);
    payload["inputRecords"] = static_cast<Json::UInt64>(records.size());
    payload["uniqueRecords"] = static_cast<Json::UInt64>(uniqueRecords.size());
    payload["duplicateRecords"] = static_cast<Json::UInt64>(records.size() - uniqueRecords.size());
    payload["changedPlayers"] = static_cast<Json::UInt64>(changedPlayers.size());
    if (!storeOperation(context->connection.get(), season, week, actor, key, "apply",
                        context->state.version, payload)
        || !commit(context->connection.get())) return statStorageUnavailable();
    return jsonResponse(payload);
}

drogon::HttpResponsePtr failStatRun(const drogon::HttpRequestPtr &request,
                                    int season,
                                    int week,
                                    const std::string &actor,
                                    const Json::Value &body) {
    const auto key = operationKey(request);
    if (key.empty()) return errorResponse(drogon::k400BadRequest, "An Idempotency-Key is required.", "idempotency_key_required");
    auto context = openStatContext(season, week);
    if (!context) return statStorageUnavailable();
    if (const auto replay = operationReplay(context->connection.get(), season, week, actor, key, "fail")) {
        auto payload = *replay;
        if (!payload["operationTypeMatches"].asBool()) {
            rollback(context->connection.get());
            return errorResponse(drogon::k409Conflict, "This idempotency key was used for another action.", "idempotency_key_conflict");
        }
        payload.removeMember("operationTypeMatches"); payload.removeMember("storedOperationType");
        if (!commit(context->connection.get())) return statStorageUnavailable();
        return jsonResponse(payload);
    }
    if (!expectedVersionMatches(body, context->state.version)) {
        auto details = statConflictDetails(context->connection.get(), context->state, actor);
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict, "Ingestion state changed.", "stat_ingestion_state_conflict", false, details);
    }
    const auto runId = int64Value(body.get("runId", 0), 0);
    const auto owner = requestedOwner(body, actor);
    const auto run = runRecord(context->connection.get(), runId);
    if (!runOwnedAndActive(run, owner) || context->state.activeRunId != runId) {
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict, "The ingestion lease is not owned by this worker.", "ingestion_lease_lost");
    }
    const auto providerStatus = std::max(0, body.get("providerStatus", 0).asInt());
    const auto networkFailure = body.get("networkFailure", false).asBool();
    const auto retryable = cff::stat_ingestion_lifecycle::retryableProviderFailure(providerStatus, networkFailure);
    const auto retryAfter = std::max(0, body.get("retryAfterSeconds", 0).asInt());
    const auto delay = retryable
        ? cff::stat_ingestion_lifecycle::retryDelaySeconds(run.attempt, retryAfter) : 0;
    const auto status = retryable ? "retry_wait" : "failed";
    auto runUpdate = execute(context->connection.get(),
        "UPDATE ingestion_runs SET status = $2, finished_at = NOW(), lease_expires_at = NOW(), "
        "provider_status = NULLIF($3::int, 0), next_retry_at = CASE WHEN $4::int > 0 "
        "THEN NOW() + make_interval(secs => $4::int) ELSE NULL END, error_message = $5 "
        "WHERE id = $1::bigint AND status = 'running' RETURNING id",
        {std::to_string(runId), status, std::to_string(providerStatus), std::to_string(delay),
         trim(body.get("error", "Provider ingestion failed").asString())});
    if (!tuplesOk(runUpdate) || PQntuples(runUpdate.get()) == 0) {
        rollback(context->connection.get()); return statStorageUnavailable();
    }
    auto stateUpdate = execute(context->connection.get(),
        "UPDATE stat_ingestion_states SET version = version + 1, status = $3, active_run_id = NULL, "
        "next_retry_at = CASE WHEN $4::int > 0 THEN NOW() + make_interval(secs => $4::int) ELSE NULL END, "
        "provider_status = NULLIF($5::int, 0), last_error = $6, updated_at = NOW() "
        "WHERE season = $1::int AND week = $2::int RETURNING version",
        {std::to_string(season), std::to_string(week), status, std::to_string(delay),
         std::to_string(providerStatus), trim(body.get("error", "Provider ingestion failed").asString())});
    if (!tuplesOk(stateUpdate) || PQntuples(stateUpdate.get()) == 0) {
        rollback(context->connection.get()); return statStorageUnavailable();
    }
    context->state = stateRecord(context->connection.get(), season, week);
    auto payload = statStatePayload(context->connection.get(), context->state, actor);
    payload["failed"] = true;
    payload["runId"] = Json::Int64(runId);
    payload["retryable"] = retryable;
    payload["retryDelaySeconds"] = delay;
    if (!storeOperation(context->connection.get(), season, week, actor, key, "fail",
                        context->state.version, payload)
        || !commit(context->connection.get())) return statStorageUnavailable();
    return jsonResponse(payload);
}

drogon::HttpResponsePtr recoverStatRun(const drogon::HttpRequestPtr &request,
                                       int season,
                                       int week,
                                       const std::string &actor,
                                       const Json::Value &body) {
    const auto key = operationKey(request);
    if (key.empty()) return errorResponse(drogon::k400BadRequest, "An Idempotency-Key is required.", "idempotency_key_required");
    auto context = openStatContext(season, week);
    if (!context) return statStorageUnavailable();
    if (!expectedVersionMatches(body, context->state.version)) {
        auto details = statConflictDetails(context->connection.get(), context->state, actor);
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict, "Ingestion state changed.", "stat_ingestion_state_conflict", false, details);
    }
    if (context->state.activeRunId > 0) {
        const auto active = runRecord(context->connection.get(), context->state.activeRunId);
        if (active.status == "running" && active.leaseActive) {
            rollback(context->connection.get());
            return errorResponse(drogon::k409Conflict, "The active worker lease has not expired.", "ingestion_run_active", true);
        }
    }
    if (!abandonExpiredRun(context->connection.get(), context->state)) {
        rollback(context->connection.get()); return statStorageUnavailable();
    }
    auto update = execute(context->connection.get(),
        "UPDATE stat_ingestion_states SET version = version + 1, status = 'idle', active_run_id = NULL, "
        "next_retry_at = NULL, provider_status = NULL, last_error = '', updated_at = NOW() "
        "WHERE season = $1::int AND week = $2::int RETURNING version",
        {std::to_string(season), std::to_string(week)});
    if (!tuplesOk(update) || PQntuples(update.get()) == 0) {
        rollback(context->connection.get()); return statStorageUnavailable();
    }
    context->state = stateRecord(context->connection.get(), season, week);
    auto payload = statStatePayload(context->connection.get(), context->state, actor);
    payload["recovered"] = true;
    if (!storeOperation(context->connection.get(), season, week, actor, key, "recover",
                        context->state.version, payload)
        || !commit(context->connection.get())) return statStorageUnavailable();
    return jsonResponse(payload);
}

drogon::HttpResponsePtr dispatchStatTransaction(const drogon::HttpRequestPtr &request,
                                                 int season,
                                                 int week,
                                                 const std::string &actor) {
    const auto bodyPointer = request->getJsonObject();
    if (!bodyPointer || !bodyPointer->isObject()) {
        return errorResponse(drogon::k400BadRequest, "An ingestion action payload is required.", "stat_ingestion_payload_required");
    }
    const auto &body = *bodyPointer;
    const auto action = lower(trim(body.get("action", "").asString()));
    if (action == "start") return startStatRun(request, season, week, actor, body);
    if (action == "heartbeat") return heartbeatStatRun(request, season, week, actor, body);
    if (action == "apply") return applyStatRun(request, season, week, actor, body);
    if (action == "fail") return failStatRun(request, season, week, actor, body);
    if (action == "recover") return recoverStatRun(request, season, week, actor, body);
    return errorResponse(drogon::k400BadRequest, "Unsupported ingestion action.", "unsupported_stat_ingestion_action");
}
