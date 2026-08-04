int tradeExpirationHours(const Json::Value &rules) {
    const auto value = rules.get("expirationHours", 48).asInt();
    return std::max(1, std::min(336, value));
}

bool tradeApprovalRequired(const Json::Value &rules) {
    return rules.get("commissionerApproval", false).asBool();
}

std::string tradeIdFor(const std::string &leagueId,
                       const std::string &email,
                       const std::string &key) {
    const auto suffix = key.empty()
        ? std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
        : key;
    return "trade-" + leagueId + "-" + email + "-" + suffix;
}

drogon::HttpResponsePtr getTradeState(const std::string &leagueId,
                                      const std::string &email,
                                      bool legacyCollection = false) {
    auto context = openTradeContext(leagueId, email);
    if (!context) return tradeStorageUnavailable();
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
    auto payload = tradeStatePayload(context->connection.get(), leagueId, email,
                                     context->access, context->version, context->expiredCount);
    if (!commit(context->connection.get())) {
        rollback(context->connection.get());
        return tradeStorageUnavailable();
    }
    return tradeStateResponse(payload, legacyCollection);
}

struct TradeSwapResult {
    bool ok{false};
    std::string errorCode;
    long long offeredByRosterVersion{-1};
    long long offeredToRosterVersion{-1};
};

TradeSwapResult executeTradeSwap(PGconn *connection,
                                 const std::string &leagueId,
                                 const TradeRecord &trade,
                                 const Json::Value &rosterRules) {
    TradeSwapResult result;
    const auto offeredOwner = playerOwner(connection, leagueId, trade.offeredPlayerId);
    const auto requestedOwner = playerOwner(connection, leagueId, trade.requestedPlayerId);
    if (!offeredOwner || *offeredOwner != trade.offeredBy
        || !requestedOwner || *requestedOwner != trade.offeredTo) {
        result.errorCode = "trade_ownership_changed";
        return result;
    }

    if (!ensureRosterState(connection, leagueId, trade.offeredBy)
        || !ensureRosterState(connection, leagueId, trade.offeredTo)) {
        result.errorCode = "roster_state_unavailable";
        return result;
    }

    const auto offeredByRoster = rosterPayload(connection, leagueId, trade.offeredBy);
    const auto offeredToRoster = rosterPayload(connection, leagueId, trade.offeredTo);
    const auto offeredDestination = cff::roster_transaction::destinationSlot(
        trade.offeredPlayer, offeredToRoster, rosterRules, trade.requestedPlayerId);
    const auto requestedDestination = cff::roster_transaction::destinationSlot(
        trade.requestedPlayer, offeredByRoster, rosterRules, trade.offeredPlayerId);
    if (!offeredDestination || !requestedDestination) {
        result.errorCode = "trade_roster_invalid";
        return result;
    }

    auto removeOffered = execute(connection,
        "DELETE FROM rosters WHERE league_id = $1 AND lower(manager_email) = lower($2) "
        "AND player_id = $3 RETURNING player_id",
        {leagueId, trade.offeredBy, trade.offeredPlayerId});
    auto removeRequested = execute(connection,
        "DELETE FROM rosters WHERE league_id = $1 AND lower(manager_email) = lower($2) "
        "AND player_id = $3 RETURNING player_id",
        {leagueId, trade.offeredTo, trade.requestedPlayerId});
    if (!tuplesOk(removeOffered) || PQntuples(removeOffered.get()) != 1
        || !tuplesOk(removeRequested) || PQntuples(removeRequested.get()) != 1) {
        result.errorCode = "trade_ownership_changed";
        return result;
    }

    auto offeredPlayer = trade.offeredPlayer;
    offeredPlayer["id"] = trade.offeredPlayerId;
    offeredPlayer["playerId"] = trade.offeredPlayerId;
    auto requestedPlayer = trade.requestedPlayer;
    requestedPlayer["id"] = trade.requestedPlayerId;
    requestedPlayer["playerId"] = trade.requestedPlayerId;

    auto addOffered = execute(connection,
        "INSERT INTO rosters "
        "(league_id, manager_email, player_id, player_snapshot, roster_slot, acquired_via, acquired_at) "
        "VALUES ($1, $2, $3, $4::jsonb, $5, 'trade', NOW()) RETURNING player_id",
        {leagueId, trade.offeredTo, trade.offeredPlayerId,
         jsonToString(offeredPlayer), *offeredDestination});
    auto addRequested = execute(connection,
        "INSERT INTO rosters "
        "(league_id, manager_email, player_id, player_snapshot, roster_slot, acquired_via, acquired_at) "
        "VALUES ($1, $2, $3, $4::jsonb, $5, 'trade', NOW()) RETURNING player_id",
        {leagueId, trade.offeredBy, trade.requestedPlayerId,
         jsonToString(requestedPlayer), *requestedDestination});
    if (!tuplesOk(addOffered) || PQntuples(addOffered.get()) != 1
        || !tuplesOk(addRequested) || PQntuples(addRequested.get()) != 1) {
        result.errorCode = "trade_roster_conflict";
        return result;
    }

    result.offeredByRosterVersion = advanceRosterVersion(connection, leagueId, trade.offeredBy);
    result.offeredToRosterVersion = advanceRosterVersion(connection, leagueId, trade.offeredTo);
    if (result.offeredByRosterVersion < 0 || result.offeredToRosterVersion < 0) {
        result.errorCode = "roster_state_unavailable";
        return result;
    }
    result.ok = true;
    return result;
}

drogon::HttpResponsePtr createTradeOffer(const drogon::HttpRequestPtr &request,
                                         const std::string &leagueId,
                                         const std::string &email,
                                         bool legacy = false) {
    const auto body = request->getJsonObject();
    if (!body || !body->isObject()) {
        return errorResponse(drogon::k400BadRequest,
                             "Trade offer details are required.",
                             "trade_payload_required");
    }
    const auto key = operationKey(request);
    if (!legacy && key.empty()) {
        return errorResponse(drogon::k400BadRequest,
                             "An Idempotency-Key is required for trade mutations.",
                             "idempotency_key_required");
    }

    auto context = openTradeContext(leagueId, email);
    if (!context) return tradeStorageUnavailable();
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
    if (lineupLocked(context->connection.get(), leagueId)) {
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict,
                             "Trades are locked after a matchup is finalized.",
                             "trades_locked");
    }

    if (const auto replay = tradeOperationReplay(
            context->connection.get(), leagueId, email, key, "create")) {
        if (!(*replay)["operationTypeMatches"].asBool()) {
            rollback(context->connection.get());
            return errorResponse(drogon::k409Conflict,
                                 "This idempotency key was already used for another trade action.",
                                 "idempotency_key_conflict");
        }
        auto payload = *replay;
        payload.removeMember("operationTypeMatches");
        payload.removeMember("storedOperationType");
        if (!commit(context->connection.get())) {
            rollback(context->connection.get());
            return tradeStorageUnavailable();
        }
        return tradeStateResponse(payload, false, payload.get("tradeId", "").asString(),
                                  legacy ? drogon::k201Created : drogon::k200OK);
    }

    if (!cff::trade_lifecycle::expectedVersionMatches(context->version, *body, !legacy)) {
        const auto details = currentTradeStateDetails(
            context->connection.get(), leagueId, email, context->access, context->version);
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict,
                             "Trade state changed. Load the latest offers before retrying.",
                             "trade_state_conflict",
                             false,
                             details);
    }

    const auto target = canonicalEmail(body->get("targetManager",
        body->get("targetManagerEmail", "")).asString());
    Json::Value offeredPlayer = (*body)["offerPlayer"];
    Json::Value requestedPlayer = (*body)["requestPlayer"];
    auto offeredPlayerId = cff::trade_lifecycle::canonicalPlayerId(
        offeredPlayer.get("id", offeredPlayer.get("playerId", "")).asString());
    auto requestedPlayerId = cff::trade_lifecycle::canonicalPlayerId(
        requestedPlayer.get("id", requestedPlayer.get("playerId", "")).asString());

    if (target.empty() || target == email || !activeTradeMember(context->connection.get(), leagueId, target)) {
        rollback(context->connection.get());
        return errorResponse(drogon::k400BadRequest,
                             "Select a different active league manager for this trade.",
                             "invalid_trade_target");
    }
    if (!cff::trade_lifecycle::validOfferPlayers(offeredPlayerId, requestedPlayerId)) {
        rollback(context->connection.get());
        return errorResponse(drogon::k400BadRequest,
                             "Both sides of the trade must include different player IDs.",
                             "invalid_trade_players");
    }

    const auto offeredOwner = playerOwner(context->connection.get(), leagueId, offeredPlayerId);
    const auto requestedOwner = playerOwner(context->connection.get(), leagueId, requestedPlayerId);
    if (!offeredOwner || *offeredOwner != email) {
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict,
                             "The offered player is no longer on your roster.",
                             "offered_player_not_owned");
    }
    if (!requestedOwner || *requestedOwner != target) {
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict,
                             "The requested player is no longer on the target manager's roster.",
                             "requested_player_not_owned");
    }

    if (const auto locked = lockedTradeOffer(context->connection.get(), leagueId, offeredPlayerId)) {
        Json::Value details(Json::objectValue);
        details["conflictingTradeId"] = *locked;
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict,
                             "The offered player is already locked in another open trade.",
                             "trade_player_locked",
                             false,
                             details);
    }
    if (const auto locked = lockedTradeOffer(context->connection.get(), leagueId, requestedPlayerId)) {
        Json::Value details(Json::objectValue);
        details["conflictingTradeId"] = *locked;
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict,
                             "The requested player is already locked in another open trade.",
                             "trade_player_locked",
                             false,
                             details);
    }

    offeredPlayer["id"] = offeredPlayerId;
    offeredPlayer["playerId"] = offeredPlayerId;
    requestedPlayer["id"] = requestedPlayerId;
    requestedPlayer["playerId"] = requestedPlayerId;
    const auto tradeId = tradeIdFor(leagueId, email, key);
    const auto requiresApproval = tradeApprovalRequired(context->access.tradeRules);
    const auto expirationHours = tradeExpirationHours(context->access.tradeRules);
    const auto note = body->get("note", "").asString().substr(0, 1000);
    const auto requestName = body->get(
        "requestPlayerName", requestedPlayer.get("name", requestedPlayerId)).asString();

    auto inserted = execute(context->connection.get(),
        "INSERT INTO trade_offers "
        "(id, league_id, offered_by_email, offered_to_email, offered_player_ids, requested_player_ids, "
        "offer_player_snapshot, request_player_snapshot, request_player_name, target_manager, note, "
        "requires_approval, status, expires_at, created_at, updated_at) "
        "VALUES ($1, $2, $3, $4, ARRAY[$5], ARRAY[$6], $7::jsonb, $8::jsonb, $9, $4, $10, "
        "$11::boolean, 'pending', NOW() + ($12::int * INTERVAL '1 hour'), NOW(), NOW()) RETURNING id",
        {tradeId, leagueId, email, target, offeredPlayerId, requestedPlayerId,
         jsonToString(offeredPlayer), jsonToString(requestedPlayer), requestName, note,
         requiresApproval ? "true" : "false", std::to_string(expirationHours)});
    if (!tuplesOk(inserted) || PQntuples(inserted.get()) != 1
        || !acquireTradePlayerLocks(context->connection.get(), leagueId, tradeId,
                                    email, offeredPlayerId, target, requestedPlayerId)) {
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict,
                             "One of the players became locked before the offer was saved.",
                             "trade_player_locked");
    }

    const auto nextVersion = advanceTradeVersion(context->connection.get(), leagueId);
    if (nextVersion < 0) {
        rollback(context->connection.get());
        return tradeStorageUnavailable();
    }
    auto versioned = execute(context->connection.get(),
        "UPDATE trade_offers SET state_version = $3::bigint, updated_at = NOW() "
        "WHERE league_id = $1 AND id = $2",
        {leagueId, tradeId, std::to_string(nextVersion)});
    if (!commandOk(versioned)) {
        rollback(context->connection.get());
        return tradeStorageUnavailable();
    }

    Json::Value metadata(Json::objectValue);
    metadata["tradeId"] = tradeId;
    metadata["offeredPlayerId"] = offeredPlayerId;
    metadata["requestedPlayerId"] = requestedPlayerId;
    metadata["targetManager"] = target;
    metadata["requiresApproval"] = requiresApproval;
    metadata["version"] = Json::Int64(nextVersion);
    if (!addTradeTransactionRecord(context->connection.get(), leagueId, email, key,
                                   "Trade Offer",
                                   "Offered " + offeredPlayer.get("name", offeredPlayerId).asString()
                                       + " for " + requestedPlayer.get("name", requestedPlayerId).asString(),
                                   metadata)) {
        rollback(context->connection.get());
        return tradeStorageUnavailable();
    }

    auto payload = tradeStatePayload(context->connection.get(), leagueId, email,
                                     context->access, nextVersion);
    payload["action"] = "create";
    payload["tradeId"] = tradeId;
    payload["operationKey"] = key;
    if (!recordTradeOperation(context->connection.get(), leagueId, email, key,
                              "create", nextVersion, payload)) {
        rollback(context->connection.get());
        return tradeStorageUnavailable();
    }
    if (!commit(context->connection.get())) {
        rollback(context->connection.get());
        return tradeStorageUnavailable();
    }
    return tradeStateResponse(payload, false, legacy ? tradeId : "",
                              legacy ? drogon::k201Created : drogon::k200OK);
}

drogon::HttpResponsePtr updateTradeLifecycleStatus(const drogon::HttpRequestPtr &request,
                                                   const std::string &leagueId,
                                                   const std::string &email,
                                                   const std::string &tradeId,
                                                   bool legacy = false) {
    const auto body = request->getJsonObject();
    if (!body || !body->isObject()) {
        return errorResponse(drogon::k400BadRequest,
                             "A trade status is required.",
                             "trade_status_required");
    }
    const auto requestedStatus = lower(trim(body->get("status", "").asString()));
    if (requestedStatus.empty()) {
        return errorResponse(drogon::k400BadRequest,
                             "A trade status is required.",
                             "trade_status_required");
    }
    const auto operationType = "status:" + requestedStatus;
    const auto key = operationKey(request);
    if (!legacy && key.empty()) {
        return errorResponse(drogon::k400BadRequest,
                             "An Idempotency-Key is required for trade mutations.",
                             "idempotency_key_required");
    }

    auto context = openTradeContext(leagueId, email);
    if (!context) return tradeStorageUnavailable();
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

    if (const auto replay = tradeOperationReplay(
            context->connection.get(), leagueId, email, key, operationType)) {
        if (!(*replay)["operationTypeMatches"].asBool()) {
            rollback(context->connection.get());
            return errorResponse(drogon::k409Conflict,
                                 "This idempotency key was already used for another trade action.",
                                 "idempotency_key_conflict");
        }
        auto payload = *replay;
        payload.removeMember("operationTypeMatches");
        payload.removeMember("storedOperationType");
        if (!commit(context->connection.get())) {
            rollback(context->connection.get());
            return tradeStorageUnavailable();
        }
        return tradeStateResponse(payload, false, legacy ? tradeId : "");
    }

    if (!cff::trade_lifecycle::expectedVersionMatches(context->version, *body, !legacy)) {
        const auto details = currentTradeStateDetails(
            context->connection.get(), leagueId, email, context->access, context->version);
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict,
                             "Trade state changed. Load the latest offers before retrying.",
                             "trade_state_conflict",
                             false,
                             details);
    }

    const auto trade = tradeForUpdate(context->connection.get(), leagueId, tradeId);
    if (!trade.found) {
        rollback(context->connection.get());
        return errorResponse(drogon::k404NotFound,
                             "Trade offer not found.",
                             "trade_not_found");
    }
    const auto involved = email == trade.offeredBy || email == trade.offeredTo;
    if (!involved && !context->access.commissioner) {
        rollback(context->connection.get());
        return errorResponse(drogon::k403Forbidden,
                             "Only trade participants or the commissioner may update this offer.",
                             "trade_access_required");
    }

    const auto decision = cff::trade_lifecycle::decideTransition(
        trade.status, requestedStatus, email, trade.offeredBy, trade.offeredTo,
        context->access.commissioner, trade.requiresApproval);
    if (!decision.allowed) {
        const auto details = currentTradeStateDetails(
            context->connection.get(), leagueId, email, context->access, context->version);
        rollback(context->connection.get());
        const auto code = decision.errorCode.empty() ? "trade_transition_conflict" : decision.errorCode;
        return errorResponse(code == "commissioner_required" ? drogon::k403Forbidden : drogon::k409Conflict,
                             "The requested trade transition is no longer allowed.",
                             code,
                             false,
                             details);
    }

    if ((decision.execute || decision.nextStatus == "accepted")
        && lineupLocked(context->connection.get(), leagueId)) {
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict,
                             "Trades are locked after a matchup is finalized.",
                             "trades_locked");
    }

    TradeSwapResult swap;
    if (decision.execute) {
        swap = executeTradeSwap(context->connection.get(), leagueId, trade,
                                context->access.rosterRules);
        if (!swap.ok) {
            const auto details = currentTradeStateDetails(
                context->connection.get(), leagueId, email, context->access, context->version);
            rollback(context->connection.get());
            return errorResponse(drogon::k409Conflict,
                                 "The trade could not be completed because roster ownership or capacity changed.",
                                 swap.errorCode.empty() ? "trade_roster_conflict" : swap.errorCode,
                                 false,
                                 details);
        }
    }

    const auto nextVersion = advanceTradeVersion(context->connection.get(), leagueId);
    if (nextVersion < 0) {
        rollback(context->connection.get());
        return tradeStorageUnavailable();
    }
    const auto terminal = cff::trade_lifecycle::terminalStatus(decision.nextStatus);
    auto updated = execute(context->connection.get(),
        "UPDATE trade_offers SET status = $3, state_version = $4::bigint, "
        "accepted_at = CASE WHEN $3 = 'accepted' THEN COALESCE(accepted_at, NOW()) ELSE accepted_at END, "
        "approved_at = CASE WHEN $3 = 'approved' THEN COALESCE(approved_at, NOW()) ELSE approved_at END, "
        "resolved_at = CASE WHEN $5::boolean THEN COALESCE(resolved_at, NOW()) ELSE resolved_at END, "
        "resolution_reason = $6, updated_at = NOW() "
        "WHERE league_id = $1 AND id = $2 AND status = $7 RETURNING id",
        {leagueId, tradeId, decision.nextStatus, std::to_string(nextVersion),
         terminal ? "true" : "false", requestedStatus, trade.status});
    if (!tuplesOk(updated) || PQntuples(updated.get()) != 1) {
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict,
                             "The trade changed before the update completed.",
                             "trade_transition_conflict");
    }
    if (decision.releaseLocks
        && !releaseTradePlayerLocks(context->connection.get(), leagueId, tradeId)) {
        rollback(context->connection.get());
        return tradeStorageUnavailable();
    }

    Json::Value metadata(Json::objectValue);
    metadata["tradeId"] = tradeId;
    metadata["status"] = decision.nextStatus;
    metadata["version"] = Json::Int64(nextVersion);
    metadata["executed"] = decision.execute;
    if (decision.execute) {
        metadata["offeredByRosterVersion"] = Json::Int64(swap.offeredByRosterVersion);
        metadata["offeredToRosterVersion"] = Json::Int64(swap.offeredToRosterVersion);
    }
    if (!addTradeTransactionRecord(context->connection.get(), leagueId, email, key,
                                   decision.execute ? "Trade Completed" : "Trade Status",
                                   decision.nextStatus + ": "
                                       + trade.offeredPlayer.get("name", trade.offeredPlayerId).asString()
                                       + " for "
                                       + trade.requestedPlayer.get("name", trade.requestedPlayerId).asString(),
                                   metadata)) {
        rollback(context->connection.get());
        return tradeStorageUnavailable();
    }

    auto payload = tradeStatePayload(context->connection.get(), leagueId, email,
                                     context->access, nextVersion);
    payload["action"] = "status";
    payload["tradeId"] = tradeId;
    payload["status"] = tradeStatusForUi(decision.nextStatus);
    payload["executed"] = decision.execute;
    payload["operationKey"] = key;
    if (decision.execute) {
        payload["offeredByRosterVersion"] = Json::Int64(swap.offeredByRosterVersion);
        payload["offeredToRosterVersion"] = Json::Int64(swap.offeredToRosterVersion);
    }
    if (!recordTradeOperation(context->connection.get(), leagueId, email, key,
                              operationType, nextVersion, payload)) {
        rollback(context->connection.get());
        return tradeStorageUnavailable();
    }
    if (!commit(context->connection.get())) {
        rollback(context->connection.get());
        return tradeStorageUnavailable();
    }
    return tradeStateResponse(payload, false, legacy ? tradeId : "");
}

drogon::HttpResponsePtr dispatchTradeTransaction(const drogon::HttpRequestPtr &request,
                                                 const std::string &leagueId,
                                                 const std::string &email) {
    const auto body = request->getJsonObject();
    if (!body || !body->isObject()) {
        return errorResponse(drogon::k400BadRequest,
                             "Trade transaction details are required.",
                             "trade_payload_required");
    }
    const auto action = lower(trim(body->get("action", "").asString()));
    if (action == "create") return createTradeOffer(request, leagueId, email, false);
    if (action == "status" || action == "accept" || action == "approve"
        || action == "decline" || action == "cancel" || action == "veto") {
        const auto tradeId = body->get("tradeId", "").asString();
        if (tradeId.empty()) {
            return errorResponse(drogon::k400BadRequest,
                                 "A trade ID is required.",
                                 "trade_id_required");
        }
        return updateTradeLifecycleStatus(request, leagueId, email, tradeId, false);
    }
    return errorResponse(drogon::k400BadRequest,
                         "Trade action must be create or status.",
                         "invalid_trade_action");
}
