enum class WaiverAction { Create, Cancel, Reorder, ProcessOne, ProcessAll, ResetPriority };

std::string waiverActionName(WaiverAction action) {
    switch (action) {
        case WaiverAction::Create: return "create";
        case WaiverAction::Cancel: return "cancel";
        case WaiverAction::Reorder: return "reorder";
        case WaiverAction::ProcessOne: return "process_one";
        case WaiverAction::ProcessAll: return "process_all";
        case WaiverAction::ResetPriority: return "reset_priority";
    }
    return "unknown";
}

std::string newWaiverIdentifier(const std::string &prefix) {
    return prefix + "-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
}

struct WaiverContext {
    PgConnection connection;
    LeagueAccess access;
    bool commissioner{false};
    long long version{0};
};

std::optional<WaiverContext> openWaiverContext(const std::string &leagueId,
                                               const std::string &email) {
    WaiverContext context;
    context.connection = connectDb();
    if (!context.connection || !begin(context.connection.get())
        || !lockWaiverLeague(context.connection.get(), leagueId)) {
        return std::nullopt;
    }
    context.access = leagueAccess(context.connection.get(), leagueId, email);
    if (!ensureWaiverState(context.connection.get(), leagueId)
        || !ensureRosterState(context.connection.get(), leagueId, email)
        || !seedWaiverPriorities(context.connection.get(), leagueId)) {
        rollback(context.connection.get());
        return std::nullopt;
    }
    context.commissioner = isCommissioner(context.connection.get(), leagueId, email);
    context.version = waiverVersion(context.connection.get(), leagueId);
    return context;
}

drogon::HttpResponsePtr waiverStorageUnavailable() {
    return errorResponse(drogon::k503ServiceUnavailable,
                         "Waiver storage is temporarily unavailable.",
                         "waiver_storage_unavailable",
                         true);
}

drogon::HttpResponsePtr getWaiverState(const std::string &leagueId,
                                       const std::string &email) {
    auto context = openWaiverContext(leagueId, email);
    if (!context) return waiverStorageUnavailable();
    if (!context->access.exists) {
        rollback(context->connection.get());
        return errorResponse(drogon::k404NotFound, "League not found.", "league_not_found");
    }
    if (!context->access.member) {
        rollback(context->connection.get());
        return errorResponse(drogon::k403Forbidden,
                             "Active league membership is required.",
                             "league_membership_required");
    }
    auto payload = waiverStatePayload(context->connection.get(), leagueId, email,
                                      context->access, context->commissioner, context->version);
    if (!commit(context->connection.get())) {
        rollback(context->connection.get());
        return waiverStorageUnavailable();
    }
    return jsonResponse(payload);
}

bool versionAccepted(const Json::Value &body,
                     const WaiverContext &context,
                     bool legacy) {
    return cff::waiver_lifecycle::expectedVersionMatches(
        context.version, body, !legacy);
}

drogon::HttpResponsePtr replayResponse(WaiverContext &context,
                                       const std::string &leagueId,
                                       const std::string &key,
                                       const std::string &actionName,
                                       bool legacy,
                                       const std::string &legacyKind = "state") {
    const auto replay = waiverOperationReplay(
        context.connection.get(), leagueId, key, actionName);
    if (!replay) return nullptr;
    if (!(*replay)["operationTypeMatches"].asBool()) {
        rollback(context.connection.get());
        Json::Value details(Json::objectValue);
        details["storedOperationType"] = (*replay)["storedOperationType"];
        return errorResponse(drogon::k409Conflict,
                             "This idempotency key was already used for another waiver action.",
                             "idempotency_key_conflict",
                             false,
                             details);
    }
    auto payload = *replay;
    payload.removeMember("operationTypeMatches");
    payload.removeMember("storedOperationType");
    if (!commit(context.connection.get())) {
        rollback(context.connection.get());
        return waiverStorageUnavailable();
    }
    if (!legacy) return jsonResponse(payload);
    if (legacyKind == "claim") return jsonResponse(claimFromState(payload, payload.get("claimId", "").asString()));
    if (legacyKind == "claims") return jsonResponse(payload["claims"]);
    if (legacyKind == "priority") return jsonResponse(payload["priority"]);
    return jsonResponse(payload);
}

drogon::HttpResponsePtr finalizeWaiverMutation(WaiverContext &context,
                                               const std::string &leagueId,
                                               const std::string &email,
                                               const std::string &key,
                                               const std::string &actionName,
                                               Json::Value payload,
                                               long long nextVersion,
                                               bool legacy,
                                               const std::string &legacyKind = "state") {
    payload["action"] = actionName;
    payload["operationKey"] = key;
    payload["version"] = Json::Int64(nextVersion);
    if (!recordWaiverOperation(context.connection.get(), leagueId, key,
                               actionName, nextVersion, payload)) {
        rollback(context.connection.get());
        return errorResponse(drogon::k503ServiceUnavailable,
                             "Waiver replay protection could not be recorded.",
                             "waiver_operation_unavailable",
                             true);
    }
    if (!commit(context.connection.get())) {
        rollback(context.connection.get());
        return errorResponse(drogon::k503ServiceUnavailable,
                             "Waiver action could not be confirmed.",
                             "waiver_confirmation_failed",
                             true);
    }
    if (!legacy) return jsonResponse(payload);
    if (legacyKind == "claim") return jsonResponse(claimFromState(payload, payload.get("claimId", "").asString()));
    if (legacyKind == "claims") return jsonResponse(payload["claims"]);
    if (legacyKind == "priority") return jsonResponse(payload["priority"]);
    return jsonResponse(payload);
}

drogon::HttpResponsePtr createWaiverClaim(const drogon::HttpRequestPtr &request,
                                          const std::string &leagueId,
                                          const std::string &email,
                                          bool legacy) {
    const auto bodyPtr = request->getJsonObject();
    if (!bodyPtr || !bodyPtr->isObject()) {
        return errorResponse(drogon::k400BadRequest,
                             "Waiver claim details are required.",
                             "waiver_payload_required");
    }
    const auto &body = *bodyPtr;
    const auto key = operationKey(request);
    if (key.empty()) {
        return errorResponse(drogon::k400BadRequest,
                             "An Idempotency-Key is required for waiver claims.",
                             "idempotency_key_required");
    }
    auto context = openWaiverContext(leagueId, email);
    if (!context) return waiverStorageUnavailable();
    if (!context->access.exists) {
        rollback(context->connection.get());
        return errorResponse(drogon::k404NotFound, "League not found.", "league_not_found");
    }
    if (!context->access.member) {
        rollback(context->connection.get());
        return errorResponse(drogon::k403Forbidden,
                             "Active league membership is required.",
                             "league_membership_required");
    }
    if (auto replay = replayResponse(*context, leagueId, key, "create", legacy, "claim")) return replay;
    if (!versionAccepted(body, *context, legacy)) {
        auto details = currentWaiverStateDetails(context->connection.get(), leagueId, email,
                                                 context->access, context->commissioner);
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict,
                             "Waiver state changed. Reload before submitting this claim.",
                             "waiver_state_conflict",
                             false,
                             details);
    }
    if (!cff::league_waiver::modeActive(context->access.waiverRules)) {
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict,
                             "This league currently uses open free agency.",
                             "waivers_not_active");
    }
    if (lineupLocked(context->connection.get(), leagueId)) {
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict,
                             "Waivers are locked after a finalized matchup.",
                             "waivers_locked");
    }
    if (pendingManagerClaimCount(context->connection.get(), leagueId, email) >= kMaxClaimsPerManager) {
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict,
                             "Too many pending waiver claims.",
                             "waiver_claim_limit");
    }

    Json::Value player(Json::objectValue);
    if (body.isMember("addPlayer") && body["addPlayer"].isObject()) player = body["addPlayer"];
    else if (body.isMember("player") && body["player"].isObject()) player = body["player"];
    auto playerId = cff::waiver_lifecycle::canonicalPlayerId(
        player.get("id", player.get("playerId", "")).asString());
    const auto dropPlayerId = cff::waiver_lifecycle::canonicalPlayerId(
        body.get("dropPlayerId", "").asString());
    if (playerId.empty()) {
        rollback(context->connection.get());
        return errorResponse(drogon::k400BadRequest,
                             "The requested player must include an ID.",
                             "player_id_required");
    }
    player["id"] = playerId;
    player["playerId"] = playerId;

    auto duplicate = execute(context->connection.get(),
        "SELECT id FROM waiver_claims WHERE league_id = $1 "
        "AND lower(manager_email) = lower($2) AND add_player_id = $3 "
        "AND status = 'pending' LIMIT 1",
        {leagueId, email, playerId});
    if (tuplesOk(duplicate) && PQntuples(duplicate.get()) > 0) {
        rollback(context->connection.get());
        Json::Value details(Json::objectValue);
        details["claimId"] = cell(duplicate.get(), 0, 0);
        return errorResponse(drogon::k409Conflict,
                             "You already have a pending claim for that player.",
                             "duplicate_waiver_claim",
                             false,
                             details);
    }
    if (playerOwner(context->connection.get(), leagueId, playerId)) {
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict,
                             "That player is no longer available.",
                             "player_unavailable");
    }
    const auto roster = rosterPayload(context->connection.get(), leagueId, email);
    if (!dropPlayerId.empty()
        && !cff::roster_transaction::rosterContainsPlayer(roster, dropPlayerId)) {
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict,
                             "The selected drop player is no longer on your roster.",
                             "drop_player_not_rostered");
    }
    if (!cff::roster_transaction::destinationSlot(
            player, roster, context->access.rosterRules, dropPlayerId)) {
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict,
                             "Your roster is full. Select a valid player to drop with this claim.",
                             "roster_full");
    }

    auto priorityResult = execute(context->connection.get(),
        "SELECT priority FROM waiver_priorities WHERE league_id = $1 "
        "AND lower(manager_email) = lower($2) LIMIT 1",
        {leagueId, email});
    const auto priority = tuplesOk(priorityResult) && PQntuples(priorityResult.get()) > 0
        ? std::stoi(cell(priorityResult.get(), 0, 0))
        : 1;
    const auto claimOrder = nextManagerClaimOrder(context->connection.get(), leagueId, email);
    const auto claimId = newWaiverIdentifier("waiver");
    auto inserted = execute(context->connection.get(),
        "INSERT INTO waiver_claims "
        "(id, league_id, manager_email, add_player_id, add_player_snapshot, drop_player_id, "
        "priority, claim_order, status, created_at, updated_at) "
        "VALUES ($1, $2, $3, $4, $5::jsonb, NULLIF($6, ''), $7::int, $8::int, 'pending', NOW(), NOW())",
        {claimId, leagueId, email, playerId, jsonToString(player), dropPlayerId,
         std::to_string(priority), std::to_string(claimOrder)});
    if (!commandOk(inserted)) {
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict,
                             "The waiver claim could not be created because its state changed.",
                             "waiver_claim_conflict");
    }
    const auto nextVersion = advanceWaiverVersion(context->connection.get(), leagueId);
    if (nextVersion < 0) {
        rollback(context->connection.get());
        return waiverStorageUnavailable();
    }
    Json::Value metadata(Json::objectValue);
    metadata["claimId"] = claimId;
    metadata["addPlayerId"] = playerId;
    metadata["dropPlayerId"] = dropPlayerId;
    metadata["version"] = Json::Int64(nextVersion);
    if (!addWaiverTransactionRecord(context->connection.get(), leagueId, email, key,
                                    "Waiver Claim", "Claimed " + player.get("name", playerId).asString(), metadata)) {
        rollback(context->connection.get());
        return waiverStorageUnavailable();
    }
    auto payload = waiverStatePayload(context->connection.get(), leagueId, email,
                                      context->access, context->commissioner, nextVersion);
    payload["claimId"] = claimId;
    payload["message"] = "Waiver claim submitted.";
    return finalizeWaiverMutation(*context, leagueId, email, key, "create",
                                  payload, nextVersion, legacy, "claim");
}

drogon::HttpResponsePtr cancelWaiverClaim(const drogon::HttpRequestPtr &request,
                                          const std::string &leagueId,
                                          const std::string &email,
                                          const std::string &claimId,
                                          bool legacy) {
    const auto bodyPtr = request->getJsonObject();
    const Json::Value empty(Json::objectValue);
    const auto &body = bodyPtr && bodyPtr->isObject() ? *bodyPtr : empty;
    const auto key = operationKey(request);
    if (key.empty()) {
        return errorResponse(drogon::k400BadRequest,
                             "An Idempotency-Key is required for waiver cancellation.",
                             "idempotency_key_required");
    }
    auto context = openWaiverContext(leagueId, email);
    if (!context) return waiverStorageUnavailable();
    if (!context->access.exists || !context->access.member) {
        rollback(context->connection.get());
        return errorResponse(drogon::k403Forbidden,
                             "Active league membership is required.",
                             "league_membership_required");
    }
    if (auto replay = replayResponse(*context, leagueId, key, "cancel", legacy, "claims")) return replay;
    if (!versionAccepted(body, *context, legacy)) {
        auto details = currentWaiverStateDetails(context->connection.get(), leagueId, email,
                                                 context->access, context->commissioner);
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict,
                             "Waiver state changed before cancellation.",
                             "waiver_state_conflict",
                             false,
                             details);
    }
    auto claim = execute(context->connection.get(),
        "SELECT lower(manager_email) FROM waiver_claims "
        "WHERE league_id = $1 AND id = $2 AND status = 'pending' FOR UPDATE",
        {leagueId, claimId});
    if (!tuplesOk(claim) || PQntuples(claim.get()) == 0) {
        rollback(context->connection.get());
        return errorResponse(drogon::k404NotFound,
                             "Pending waiver claim not found.",
                             "waiver_claim_not_found");
    }
    const auto owner = canonicalEmail(cell(claim.get(), 0, 0));
    if (owner != email && !context->commissioner) {
        rollback(context->connection.get());
        return errorResponse(drogon::k403Forbidden,
                             "Only the claim owner or commissioner may cancel it.",
                             "waiver_claim_access_denied");
    }
    const auto failureCode = owner == email ? "cancelled_by_manager" : "cancelled_by_commissioner";
    auto updated = execute(context->connection.get(),
        "UPDATE waiver_claims SET status = 'cancelled', failure_code = $4, "
        "resolved_by_email = $3, processed_at = NOW(), updated_at = NOW() "
        "WHERE league_id = $1 AND id = $2 AND status = 'pending' RETURNING id",
        {leagueId, claimId, email, failureCode});
    if (!tuplesOk(updated) || PQntuples(updated.get()) != 1) {
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict,
                             "The waiver claim changed before cancellation.",
                             "waiver_claim_conflict");
    }
    const auto nextVersion = advanceWaiverVersion(context->connection.get(), leagueId);
    if (nextVersion < 0) {
        rollback(context->connection.get());
        return waiverStorageUnavailable();
    }
    Json::Value metadata(Json::objectValue);
    metadata["claimId"] = claimId;
    metadata["version"] = Json::Int64(nextVersion);
    if (!addWaiverTransactionRecord(context->connection.get(), leagueId, email, key,
                                    "Waiver Cancelled", "Cancelled waiver claim", metadata)) {
        rollback(context->connection.get());
        return waiverStorageUnavailable();
    }
    auto payload = waiverStatePayload(context->connection.get(), leagueId, email,
                                      context->access, context->commissioner, nextVersion);
    payload["claimId"] = claimId;
    payload["message"] = "Waiver claim cancelled.";
    return finalizeWaiverMutation(*context, leagueId, email, key, "cancel",
                                  payload, nextVersion, legacy, "claims");
}

drogon::HttpResponsePtr reorderWaiverClaims(const drogon::HttpRequestPtr &request,
                                            const std::string &leagueId,
                                            const std::string &email,
                                            bool legacy) {
    const auto bodyPtr = request->getJsonObject();
    if (!bodyPtr || !bodyPtr->isObject() || !(*bodyPtr)["claimIds"].isArray()) {
        return errorResponse(drogon::k400BadRequest,
                             "The complete pending claim order is required.",
                             "waiver_reorder_required");
    }
    const auto &body = *bodyPtr;
    const auto key = operationKey(request);
    if (key.empty()) {
        return errorResponse(drogon::k400BadRequest,
                             "An Idempotency-Key is required for waiver reordering.",
                             "idempotency_key_required");
    }
    auto context = openWaiverContext(leagueId, email);
    if (!context) return waiverStorageUnavailable();
    if (!context->access.member) {
        rollback(context->connection.get());
        return errorResponse(drogon::k403Forbidden,
                             "Active league membership is required.",
                             "league_membership_required");
    }
    if (auto replay = replayResponse(*context, leagueId, key, "reorder", legacy, "claims")) return replay;
    if (!versionAccepted(body, *context, legacy)) {
        auto details = currentWaiverStateDetails(context->connection.get(), leagueId, email,
                                                 context->access, context->commissioner);
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict,
                             "Waiver state changed before reordering.",
                             "waiver_state_conflict",
                             false,
                             details);
    }
    const auto pending = pendingManagerClaims(context->connection.get(), leagueId, email);
    if (!cff::waiver_lifecycle::validClaimReorder(pending, body["claimIds"], email)) {
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict,
                             "Claim order must include every pending claim exactly once.",
                             "waiver_reorder_conflict");
    }
    int order = 1;
    for (const auto &id : body["claimIds"]) {
        auto updated = execute(context->connection.get(),
            "UPDATE waiver_claims SET claim_order = $4::int, updated_at = NOW() "
            "WHERE league_id = $1 AND lower(manager_email) = lower($2) "
            "AND id = $3 AND status = 'pending'",
            {leagueId, email, id.asString(), std::to_string(order++)});
        if (!commandOk(updated)) {
            rollback(context->connection.get());
            return waiverStorageUnavailable();
        }
    }
    const auto nextVersion = advanceWaiverVersion(context->connection.get(), leagueId);
    if (nextVersion < 0) {
        rollback(context->connection.get());
        return waiverStorageUnavailable();
    }
    auto payload = waiverStatePayload(context->connection.get(), leagueId, email,
                                      context->access, context->commissioner, nextVersion);
    payload["message"] = "Waiver claim order saved.";
    return finalizeWaiverMutation(*context, leagueId, email, key, "reorder",
                                  payload, nextVersion, legacy, "claims");
}

struct ClaimResolution {
    std::string claimId;
    std::string managerEmail;
    std::string playerId;
    std::string status;
    std::string failureCode;
};

std::optional<Json::Value> nextPendingClaim(PGconn *connection,
                                            const std::string &leagueId) {
    auto result = execute(connection,
        "SELECT c.id, lower(c.manager_email), c.add_player_id, c.add_player_snapshot::text, "
        "COALESCE(c.drop_player_id, ''), COALESCE(w.priority, c.priority, 9999), c.claim_order "
        "FROM waiver_claims c LEFT JOIN waiver_priorities w "
        "ON w.league_id = c.league_id AND lower(w.manager_email) = lower(c.manager_email) "
        "WHERE c.league_id = $1 AND c.status = 'pending' "
        "ORDER BY COALESCE(w.priority, c.priority, 9999), c.claim_order, c.created_at, c.id "
        "LIMIT 1 FOR UPDATE OF c",
        {leagueId});
    if (!tuplesOk(result) || PQntuples(result.get()) == 0) return std::nullopt;
    Json::Value claim(Json::objectValue);
    claim["id"] = cell(result.get(), 0, 0);
    claim["managerEmail"] = canonicalEmail(cell(result.get(), 0, 1));
    claim["addPlayerId"] = cell(result.get(), 0, 2);
    claim["addPlayer"] = jsonFromString(cell(result.get(), 0, 3));
    claim["dropPlayerId"] = cell(result.get(), 0, 4);
    claim["priority"] = std::stoi(cell(result.get(), 0, 5));
    claim["claimOrder"] = std::stoi(cell(result.get(), 0, 6));
    return claim;
}

bool markClaimFailed(PGconn *connection,
                     const std::string &leagueId,
                     const std::string &claimId,
                     const std::string &actor,
                     const std::string &runId,
                     const std::string &failureCode) {
    return commandOk(execute(connection,
        "UPDATE waiver_claims SET status = 'failed', failure_code = $4, "
        "resolved_by_email = $3, resolution_run_id = $5, processed_at = NOW(), updated_at = NOW() "
        "WHERE league_id = $1 AND id = $2 AND status = 'pending'",
        {leagueId, claimId, actor, failureCode, runId}));
}

std::optional<ClaimResolution> resolveNextClaim(PGconn *connection,
                                                const std::string &leagueId,
                                                const std::string &actor,
                                                const std::string &runId,
                                                const LeagueAccess &access) {
    const auto claim = nextPendingClaim(connection, leagueId);
    if (!claim) return std::nullopt;
    ClaimResolution outcome;
    outcome.claimId = (*claim)["id"].asString();
    outcome.managerEmail = canonicalEmail((*claim)["managerEmail"].asString());
    outcome.playerId = cff::waiver_lifecycle::canonicalPlayerId((*claim)["addPlayerId"].asString());
    const auto dropId = cff::waiver_lifecycle::canonicalPlayerId((*claim)["dropPlayerId"].asString());
    auto player = (*claim)["addPlayer"];
    if (!player.isObject()) player = Json::Value{Json::objectValue};
    player["id"] = outcome.playerId;
    player["playerId"] = outcome.playerId;

    auto fail = [&](const std::string &code) -> std::optional<ClaimResolution> {
        if (!markClaimFailed(connection, leagueId, outcome.claimId, actor, runId, code)) return std::nullopt;
        outcome.status = "failed";
        outcome.failureCode = code;
        Json::Value metadata(Json::objectValue);
        metadata["claimId"] = outcome.claimId;
        metadata["playerId"] = outcome.playerId;
        metadata["failureCode"] = code;
        if (!addWaiverTransactionRecord(connection, leagueId, outcome.managerEmail,
                                        runId + "-" + outcome.claimId,
                                        "Waiver Failed",
                                        "Waiver claim failed: " + code,
                                        metadata)) return std::nullopt;
        return outcome;
    };

    if (outcome.playerId.empty()) return fail("player_id_missing");
    if (playerOwner(connection, leagueId, outcome.playerId)) return fail("player_unavailable");
    const auto roster = rosterPayload(connection, leagueId, outcome.managerEmail);
    if (!dropId.empty() && !cff::roster_transaction::rosterContainsPlayer(roster, dropId)) {
        return fail("drop_player_not_rostered");
    }
    const auto destination = cff::roster_transaction::destinationSlot(
        player, roster, access.rosterRules, dropId);
    if (!destination) return fail("roster_full");

    if (!commandOk(execute(connection, "SAVEPOINT waiver_claim_step"))) return std::nullopt;
    if (!dropId.empty()) {
        auto removed = execute(connection,
            "DELETE FROM rosters WHERE league_id = $1 AND lower(manager_email) = lower($2) "
            "AND player_id = $3 RETURNING player_id",
            {leagueId, outcome.managerEmail, dropId});
        if (!tuplesOk(removed) || PQntuples(removed.get()) != 1) {
            (void)execute(connection, "ROLLBACK TO SAVEPOINT waiver_claim_step");
            return fail("drop_player_conflict");
        }
    }
    auto inserted = execute(connection,
        "INSERT INTO rosters "
        "(league_id, manager_email, player_id, player_snapshot, roster_slot, acquired_via, acquired_at) "
        "VALUES ($1, $2, $3, $4::jsonb, $5, 'waiver', NOW()) "
        "ON CONFLICT (league_id, player_id) DO NOTHING RETURNING player_id",
        {leagueId, outcome.managerEmail, outcome.playerId, jsonToString(player), *destination});
    if (!tuplesOk(inserted) || PQntuples(inserted.get()) != 1) {
        (void)execute(connection, "ROLLBACK TO SAVEPOINT waiver_claim_step");
        return fail("player_unavailable");
    }
    auto claimUpdate = execute(connection,
        "UPDATE waiver_claims SET status = 'processed', failure_code = '', "
        "resolved_by_email = $3, resolution_run_id = $4, processed_at = NOW(), updated_at = NOW() "
        "WHERE league_id = $1 AND id = $2 AND status = 'pending' RETURNING id",
        {leagueId, outcome.claimId, actor, runId});
    if (!tuplesOk(claimUpdate) || PQntuples(claimUpdate.get()) != 1) {
        (void)execute(connection, "ROLLBACK TO SAVEPOINT waiver_claim_step");
        return std::nullopt;
    }
    if (advanceRosterVersion(connection, leagueId, outcome.managerEmail) < 0
        || !moveManagerToBackDb(connection, leagueId, outcome.managerEmail)) {
        (void)execute(connection, "ROLLBACK TO SAVEPOINT waiver_claim_step");
        return std::nullopt;
    }
    Json::Value metadata(Json::objectValue);
    metadata["claimId"] = outcome.claimId;
    metadata["addedPlayerId"] = outcome.playerId;
    metadata["droppedPlayerId"] = dropId;
    metadata["processingRunId"] = runId;
    if (!addWaiverTransactionRecord(connection, leagueId, outcome.managerEmail,
                                    runId + "-" + outcome.claimId,
                                    "Waiver Processed",
                                    "Added " + player.get("name", outcome.playerId).asString(),
                                    metadata)) {
        (void)execute(connection, "ROLLBACK TO SAVEPOINT waiver_claim_step");
        return std::nullopt;
    }
    (void)execute(connection, "RELEASE SAVEPOINT waiver_claim_step");
    outcome.status = "processed";
    return outcome;
}

drogon::HttpResponsePtr processWaiverClaims(const drogon::HttpRequestPtr &request,
                                            const std::string &leagueId,
                                            const std::string &email,
                                            const std::string &requestedClaimId,
                                            bool processAll,
                                            bool legacy) {
    const auto bodyPtr = request->getJsonObject();
    const Json::Value empty(Json::objectValue);
    const auto &body = bodyPtr && bodyPtr->isObject() ? *bodyPtr : empty;
    const auto key = operationKey(request);
    if (key.empty()) {
        return errorResponse(drogon::k400BadRequest,
                             "An Idempotency-Key is required for waiver processing.",
                             "idempotency_key_required");
    }
    auto context = openWaiverContext(leagueId, email);
    if (!context) return waiverStorageUnavailable();
    if (!context->access.exists || !context->access.member) {
        rollback(context->connection.get());
        return errorResponse(drogon::k403Forbidden,
                             "Active league membership is required.",
                             "league_membership_required");
    }
    if (!context->commissioner) {
        rollback(context->connection.get());
        return errorResponse(drogon::k403Forbidden,
                             "Only the commissioner can process waivers.",
                             "commissioner_required");
    }
    const auto actionName = processAll ? "process_all" : "process_one";
    if (auto replay = replayResponse(*context, leagueId, key, actionName, legacy,
                                     processAll ? "state" : "claim")) return replay;
    if (!versionAccepted(body, *context, legacy)) {
        auto details = currentWaiverStateDetails(context->connection.get(), leagueId, email,
                                                 context->access, context->commissioner);
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict,
                             "Waiver state changed before processing.",
                             "waiver_state_conflict",
                             false,
                             details);
    }
    if (lineupLocked(context->connection.get(), leagueId)) {
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict,
                             "Waivers are locked after a finalized matchup.",
                             "waivers_locked");
    }
    if (!cff::league_waiver::deadlinePassed(context->access.waiverRules)) {
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict,
                             "The waiver deadline has not passed.",
                             "waiver_deadline_pending");
    }
    if (!processAll) {
        const auto next = nextPendingClaim(context->connection.get(), leagueId);
        if (!next) {
            rollback(context->connection.get());
            return errorResponse(drogon::k404NotFound,
                                 "No pending waiver claim was found.",
                                 "waiver_claim_not_found");
        }
        if ((*next)["id"].asString() != requestedClaimId) {
            Json::Value details(Json::objectValue);
            details["nextClaimId"] = (*next)["id"];
            details["state"] = waiverStatePayload(context->connection.get(), leagueId, email,
                                                   context->access, context->commissioner, context->version);
            rollback(context->connection.get());
            return errorResponse(drogon::k409Conflict,
                                 "Claims must be processed in the current priority order.",
                                 "waiver_claim_out_of_order",
                                 false,
                                 details);
        }
    }

    const auto runId = key.empty() ? newWaiverIdentifier("waiver-run") : key;
    Json::Value processed(Json::arrayValue);
    Json::Value failed(Json::arrayValue);
    int iterations = 0;
    do {
        auto outcome = resolveNextClaim(context->connection.get(), leagueId, email, runId, context->access);
        if (!outcome) {
            if (!nextPendingClaim(context->connection.get(), leagueId)) break;
            rollback(context->connection.get());
            return errorResponse(drogon::k503ServiceUnavailable,
                                 "Waiver processing could not complete safely.",
                                 "waiver_processing_failed",
                                 true);
        }
        if (outcome->status == "processed") processed.append(outcome->claimId);
        else failed.append(outcome->claimId);
        ++iterations;
        if (!processAll) break;
    } while (iterations < 1000 && nextPendingClaim(context->connection.get(), leagueId));

    if (iterations >= 1000 && nextPendingClaim(context->connection.get(), leagueId)) {
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict,
                             "The waiver run exceeded its safe claim limit.",
                             "waiver_processing_limit");
    }
    const auto nextVersion = advanceWaiverVersion(context->connection.get(), leagueId, runId);
    if (nextVersion < 0) {
        rollback(context->connection.get());
        return waiverStorageUnavailable();
    }
    auto payload = waiverStatePayload(context->connection.get(), leagueId, email,
                                      context->access, context->commissioner, nextVersion);
    payload["processingRunId"] = runId;
    payload["processed"] = processed;
    payload["failed"] = failed;
    payload["cancelled"] = failed;
    payload["message"] = processAll ? "Waiver run completed." : "Waiver claim resolved.";
    if (!processAll) payload["claimId"] = requestedClaimId;
    return finalizeWaiverMutation(*context, leagueId, email, key, actionName,
                                  payload, nextVersion, legacy,
                                  processAll ? "state" : "claim");
}

drogon::HttpResponsePtr resetWaiverPriority(const drogon::HttpRequestPtr &request,
                                            const std::string &leagueId,
                                            const std::string &email,
                                            bool legacy) {
    const auto bodyPtr = request->getJsonObject();
    const Json::Value empty(Json::objectValue);
    const auto &body = bodyPtr && bodyPtr->isObject() ? *bodyPtr : empty;
    const auto key = operationKey(request);
    if (key.empty()) {
        return errorResponse(drogon::k400BadRequest,
                             "An Idempotency-Key is required for priority reset.",
                             "idempotency_key_required");
    }
    auto context = openWaiverContext(leagueId, email);
    if (!context) return waiverStorageUnavailable();
    if (!context->commissioner) {
        rollback(context->connection.get());
        return errorResponse(drogon::k403Forbidden,
                             "Only the commissioner can reset waiver priority.",
                             "commissioner_required");
    }
    if (auto replay = replayResponse(*context, leagueId, key, "reset_priority", legacy, "priority")) return replay;
    if (!versionAccepted(body, *context, legacy)) {
        auto details = currentWaiverStateDetails(context->connection.get(), leagueId, email,
                                                 context->access, context->commissioner);
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict,
                             "Waiver state changed before priority reset.",
                             "waiver_state_conflict",
                             false,
                             details);
    }
    if (!commandOk(execute(context->connection.get(),
                           "DELETE FROM waiver_priorities WHERE league_id = $1",
                           {leagueId}))
        || !seedWaiverPriorities(context->connection.get(), leagueId)) {
        rollback(context->connection.get());
        return waiverStorageUnavailable();
    }
    const auto nextVersion = advanceWaiverVersion(context->connection.get(), leagueId);
    if (nextVersion < 0) {
        rollback(context->connection.get());
        return waiverStorageUnavailable();
    }
    Json::Value metadata(Json::objectValue);
    metadata["version"] = Json::Int64(nextVersion);
    if (!addWaiverTransactionRecord(context->connection.get(), leagueId, email, key,
                                    "Waiver Priority", "Reset waiver priority order", metadata)) {
        rollback(context->connection.get());
        return waiverStorageUnavailable();
    }
    auto payload = waiverStatePayload(context->connection.get(), leagueId, email,
                                      context->access, context->commissioner, nextVersion);
    payload["message"] = "Waiver priority reset.";
    return finalizeWaiverMutation(*context, leagueId, email, key, "reset_priority",
                                  payload, nextVersion, legacy, "priority");
}

drogon::HttpResponsePtr dispatchWaiverTransaction(const drogon::HttpRequestPtr &request,
                                                  const std::string &leagueId,
                                                  const std::string &email) {
    const auto body = request->getJsonObject();
    if (!body || !body->isObject()) {
        return errorResponse(drogon::k400BadRequest,
                             "Waiver action details are required.",
                             "waiver_payload_required");
    }
    const auto action = lower(trim(body->get("action", "").asString()));
    if (action == "create") return createWaiverClaim(request, leagueId, email, false);
    if (action == "cancel") {
        return cancelWaiverClaim(request, leagueId, email,
                                 body->get("claimId", "").asString(), false);
    }
    if (action == "reorder") return reorderWaiverClaims(request, leagueId, email, false);
    if (action == "process") return processWaiverClaims(request, leagueId, email, "", true, false);
    if (action == "process_one") {
        return processWaiverClaims(request, leagueId, email,
                                   body->get("claimId", "").asString(), false, false);
    }
    if (action == "reset_priority") return resetWaiverPriority(request, leagueId, email, false);
    return errorResponse(drogon::k400BadRequest,
                         "Waiver action must be create, cancel, reorder, process, process_one, or reset_priority.",
                         "invalid_waiver_action");
}
