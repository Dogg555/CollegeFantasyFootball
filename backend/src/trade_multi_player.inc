constexpr std::size_t kMaximumTradePlayersPerSide = 20;

std::string tradePlayerId(const Json::Value &player) {
    if (!player.isObject()) return "";
    return cff::trade_lifecycle::canonicalPlayerId(
        player.get("id", player.get("playerId", "")).asString());
}

Json::Value tradePackageInput(const Json::Value &body,
                              const char *pluralKey,
                              const char *singularKey) {
    if (body.isMember(pluralKey) && body[pluralKey].isArray()) {
        return body[pluralKey];
    }
    Json::Value package(Json::arrayValue);
    if (body.isMember(singularKey) && body[singularKey].isObject()) {
        package.append(body[singularKey]);
    }
    return package;
}

bool normalizeTradePackage(const Json::Value &input,
                           Json::Value &players,
                           std::vector<std::string> &ids) {
    players = Json::Value(Json::arrayValue);
    ids.clear();
    if (!input.isArray() || input.empty() || input.size() > kMaximumTradePlayersPerSide) {
        return false;
    }
    std::unordered_set<std::string> seen;
    for (const auto &rawPlayer : input) {
        if (!rawPlayer.isObject()) return false;
        auto player = rawPlayer;
        const auto id = tradePlayerId(player);
        if (id.empty() || !seen.insert(id).second) return false;
        player["id"] = id;
        player["playerId"] = id;
        players.append(player);
        ids.push_back(id);
    }
    return true;
}

Json::Value idArray(const std::vector<std::string> &ids) {
    Json::Value values(Json::arrayValue);
    for (const auto &id : ids) values.append(id);
    return values;
}

Json::Value rosterWithoutPlayers(const Json::Value &roster,
                                 const std::vector<std::string> &removedIds) {
    std::unordered_set<std::string> removed(removedIds.begin(), removedIds.end());
    Json::Value remaining(Json::arrayValue);
    if (!roster.isArray()) return remaining;
    for (const auto &player : roster) {
        if (!removed.count(tradePlayerId(player))) remaining.append(player);
    }
    return remaining;
}

std::optional<Json::Value> rosterPlayer(const Json::Value &roster,
                                        const std::string &playerId) {
    if (!roster.isArray()) return std::nullopt;
    for (const auto &player : roster) {
        if (tradePlayerId(player) == playerId) return player;
    }
    return std::nullopt;
}

bool authoritativeTradePackage(const Json::Value &submitted,
                               const Json::Value &roster,
                               Json::Value &players,
                               std::vector<std::string> &ids) {
    Json::Value normalized;
    std::vector<std::string> normalizedIds;
    if (!normalizeTradePackage(submitted, normalized, normalizedIds)) return false;
    players = Json::Value(Json::arrayValue);
    ids.clear();
    for (const auto &id : normalizedIds) {
        const auto owned = rosterPlayer(roster, id);
        if (!owned) return false;
        auto player = *owned;
        player["id"] = id;
        player["playerId"] = id;
        players.append(player);
        ids.push_back(id);
    }
    return true;
}

std::string tradePackageNames(const Json::Value &players) {
    std::string names;
    if (!players.isArray()) return names;
    for (const auto &player : players) {
        if (!names.empty()) names += ", ";
        const auto id = tradePlayerId(player);
        names += player.get("name", id.empty() ? "player" : id).asString();
    }
    return names;
}

Json::Value storedTradePackage(const std::string &idsRaw,
                               const std::string &snapshotsRaw) {
    auto ids = jsonFromString(idsRaw, Json::Value{Json::arrayValue});
    if (!ids.isArray()) ids = Json::Value(Json::arrayValue);
    auto snapshots = jsonFromString(snapshotsRaw, Json::Value{Json::arrayValue});
    if (snapshots.isObject()) {
        Json::Value array(Json::arrayValue);
        array.append(snapshots);
        snapshots = array;
    }
    if (!snapshots.isArray()) snapshots = Json::Value(Json::arrayValue);

    Json::Value players(Json::arrayValue);
    for (Json::ArrayIndex index = 0; index < ids.size(); ++index) {
        const auto id = cff::trade_lifecycle::canonicalPlayerId(ids[index].asString());
        Json::Value player(Json::objectValue);
        for (const auto &candidate : snapshots) {
            if (tradePlayerId(candidate) == id) {
                player = candidate;
                break;
            }
        }
        if (player.empty() && index < snapshots.size() && snapshots[index].isObject()) {
            player = snapshots[index];
        }
        player["id"] = id;
        player["playerId"] = id;
        players.append(player);
    }
    return players;
}

Json::Value tradeOfferFromRowPackage(PGresult *result, int row) {
    Json::Value offer(Json::objectValue);
    offer["id"] = cell(result, row, 0);
    offer["offeredByEmail"] = canonicalEmail(cell(result, row, 1));
    offer["offeredToEmail"] = canonicalEmail(cell(result, row, 2));
    const auto offeredPlayers = storedTradePackage(cell(result, row, 3), cell(result, row, 4));
    const auto requestedPlayers = storedTradePackage(cell(result, row, 5), cell(result, row, 6));
    offer["offerPlayers"] = offeredPlayers;
    offer["requestPlayers"] = requestedPlayers;
    offer["offerPlayer"] = offeredPlayers.empty() ? Json::Value(Json::objectValue) : offeredPlayers[0];
    offer["requestPlayer"] = requestedPlayers.empty() ? Json::Value(Json::objectValue) : requestedPlayers[0];
    offer["requestPlayerName"] = cell(result, row, 7);
    offer["targetManager"] = canonicalEmail(cell(result, row, 8));
    offer["note"] = cell(result, row, 9);
    offer["requiresApproval"] = cell(result, row, 10) == "t";
    const auto status = cell(result, row, 11);
    const auto resolutionReason = cell(result, row, 18);
    offer["status"] = status == "declined" && resolutionReason.rfind("countered", 0) == 0
        ? "Countered"
        : tradeStatusForUi(status);
    offer["createdAt"] = cell(result, row, 12);
    offer["expiresAt"] = cell(result, row, 13);
    offer["resolvedAt"] = cell(result, row, 14);
    offer["acceptedAt"] = cell(result, row, 15);
    offer["approvedAt"] = cell(result, row, 16);
    offer["stateVersion"] = Json::Int64(cellInt64(result, row, 17, 0));
    offer["resolutionReason"] = resolutionReason;
    offer["offerSummary"] = tradePackageNames(offeredPlayers);
    offer["requestSummary"] = tradePackageNames(requestedPlayers);
    return offer;
}

Json::Value tradeOffersPayloadPackage(PGconn *connection,
                                      const std::string &leagueId,
                                      const std::string &email,
                                      bool commissioner) {
    auto result = execute(connection,
        "SELECT id, lower(offered_by_email), lower(offered_to_email), "
        "COALESCE(to_json(offered_player_ids)::text, '[]'), offer_player_snapshot::text, "
        "COALESCE(to_json(requested_player_ids)::text, '[]'), request_player_snapshot::text, "
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
        offers.append(tradeOfferFromRowPackage(result.get(), row));
    }
    return offers;
}

Json::Value tradeStatePayloadPackage(PGconn *connection,
                                     const std::string &leagueId,
                                     const std::string &email,
                                     const TradeAccess &access,
                                     long long version,
                                     int expiredCount = 0) {
    Json::Value payload(Json::objectValue);
    payload["leagueId"] = leagueId;
    payload["version"] = Json::Int64(version);
    payload["offers"] = tradeOffersPayloadPackage(connection, leagueId, email, access.commissioner);
    payload["roster"] = rosterPayload(connection, leagueId, email);
    payload["tradeRules"] = access.tradeRules;
    payload["commissioner"] = access.commissioner;
    payload["expiredCount"] = expiredCount;
    payload["lineupLocked"] = lineupLocked(connection, leagueId);
    payload["multiPlayerTrades"] = true;
    return payload;
}

Json::Value currentTradeStateDetailsPackage(PGconn *connection,
                                            const std::string &leagueId,
                                            const std::string &email,
                                            const TradeAccess &access,
                                            long long version) {
    Json::Value details(Json::objectValue);
    details["currentVersion"] = Json::Int64(version);
    details["state"] = tradeStatePayloadPackage(connection, leagueId, email, access, version);
    return details;
}

drogon::HttpResponsePtr getTradeStatePackage(const std::string &leagueId,
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
    auto payload = tradeStatePayloadPackage(context->connection.get(), leagueId, email,
                                            context->access, context->version, context->expiredCount);
    if (!commit(context->connection.get())) {
        rollback(context->connection.get());
        return tradeStorageUnavailable();
    }
    return tradeStateResponse(payload, legacyCollection);
}

bool acquireTradePlayerLocksPackage(PGconn *connection,
                                    const std::string &leagueId,
                                    const std::string &offerId,
                                    const std::string &offeredBy,
                                    const std::vector<std::string> &offeredPlayerIds,
                                    const std::string &offeredTo,
                                    const std::vector<std::string> &requestedPlayerIds) {
    for (const auto &playerId : offeredPlayerIds) {
        auto inserted = execute(connection,
            "INSERT INTO trade_player_locks "
            "(league_id, player_id, offer_id, manager_email, lock_role, created_at) "
            "VALUES ($1, $2, $3, $4, 'offered', NOW())",
            {leagueId, playerId, offerId, offeredBy});
        if (!commandOk(inserted)) return false;
    }
    for (const auto &playerId : requestedPlayerIds) {
        auto inserted = execute(connection,
            "INSERT INTO trade_player_locks "
            "(league_id, player_id, offer_id, manager_email, lock_role, created_at) "
            "VALUES ($1, $2, $3, $4, 'requested', NOW())",
            {leagueId, playerId, offerId, offeredTo});
        if (!commandOk(inserted)) return false;
    }
    return true;
}

struct TradeRecordPackage {
    bool found{false};
    std::string id;
    std::string offeredBy;
    std::string offeredTo;
    std::vector<std::string> offeredPlayerIds;
    std::vector<std::string> requestedPlayerIds;
    Json::Value offeredPlayers{Json::arrayValue};
    Json::Value requestedPlayers{Json::arrayValue};
    std::string status;
    bool requiresApproval{false};
};

std::vector<std::string> packageIds(const Json::Value &players) {
    std::vector<std::string> ids;
    if (!players.isArray()) return ids;
    for (const auto &player : players) ids.push_back(tradePlayerId(player));
    return ids;
}

TradeRecordPackage tradeForUpdatePackage(PGconn *connection,
                                         const std::string &leagueId,
                                         const std::string &tradeId) {
    TradeRecordPackage trade;
    auto result = execute(connection,
        "SELECT id, lower(offered_by_email), lower(offered_to_email), "
        "COALESCE(to_json(offered_player_ids)::text, '[]'), "
        "COALESCE(to_json(requested_player_ids)::text, '[]'), "
        "offer_player_snapshot::text, request_player_snapshot::text, status, requires_approval "
        "FROM trade_offers WHERE league_id = $1 AND id = $2 LIMIT 1 FOR UPDATE",
        {leagueId, tradeId});
    if (!tuplesOk(result) || PQntuples(result.get()) == 0) return trade;
    trade.found = true;
    trade.id = cell(result.get(), 0, 0);
    trade.offeredBy = canonicalEmail(cell(result.get(), 0, 1));
    trade.offeredTo = canonicalEmail(cell(result.get(), 0, 2));
    trade.offeredPlayers = storedTradePackage(cell(result.get(), 0, 3), cell(result.get(), 0, 5));
    trade.requestedPlayers = storedTradePackage(cell(result.get(), 0, 4), cell(result.get(), 0, 6));
    trade.offeredPlayerIds = packageIds(trade.offeredPlayers);
    trade.requestedPlayerIds = packageIds(trade.requestedPlayers);
    trade.status = lower(cell(result.get(), 0, 7));
    trade.requiresApproval = cell(result.get(), 0, 8) == "t";
    return trade;
}

std::optional<std::vector<std::pair<Json::Value, std::string>>> planIncomingTradePackage(
    const Json::Value &incomingPlayers,
    Json::Value roster,
    const Json::Value &rosterRules) {
    std::vector<std::pair<Json::Value, std::string>> planned;
    for (const auto &incoming : incomingPlayers) {
        const auto slot = cff::roster_transaction::destinationSlot(
            incoming, roster, rosterRules, "");
        if (!slot) return std::nullopt;
        auto player = incoming;
        player["rosterSlot"] = *slot;
        roster.append(player);
        planned.emplace_back(player, *slot);
    }
    return planned;
}

struct TradeSwapResultPackage {
    bool ok{false};
    std::string errorCode;
    long long offeredByRosterVersion{-1};
    long long offeredToRosterVersion{-1};
};

TradeSwapResultPackage executeTradeSwapPackage(PGconn *connection,
                                               const std::string &leagueId,
                                               const TradeRecordPackage &trade,
                                               const Json::Value &rosterRules) {
    TradeSwapResultPackage result;
    for (const auto &playerId : trade.offeredPlayerIds) {
        const auto owner = playerOwner(connection, leagueId, playerId);
        if (!owner || *owner != trade.offeredBy) {
            result.errorCode = "trade_ownership_changed";
            return result;
        }
    }
    for (const auto &playerId : trade.requestedPlayerIds) {
        const auto owner = playerOwner(connection, leagueId, playerId);
        if (!owner || *owner != trade.offeredTo) {
            result.errorCode = "trade_ownership_changed";
            return result;
        }
    }
    if (!ensureRosterState(connection, leagueId, trade.offeredBy)
        || !ensureRosterState(connection, leagueId, trade.offeredTo)) {
        result.errorCode = "roster_state_unavailable";
        return result;
    }

    const auto offeredByRoster = rosterWithoutPlayers(
        rosterPayload(connection, leagueId, trade.offeredBy), trade.offeredPlayerIds);
    const auto offeredToRoster = rosterWithoutPlayers(
        rosterPayload(connection, leagueId, trade.offeredTo), trade.requestedPlayerIds);
    const auto requestedPlan = planIncomingTradePackage(
        trade.requestedPlayers, offeredByRoster, rosterRules);
    const auto offeredPlan = planIncomingTradePackage(
        trade.offeredPlayers, offeredToRoster, rosterRules);
    if (!requestedPlan || !offeredPlan) {
        result.errorCode = "trade_roster_invalid";
        return result;
    }

    const auto offeredIdsJson = jsonToString(idArray(trade.offeredPlayerIds));
    const auto requestedIdsJson = jsonToString(idArray(trade.requestedPlayerIds));
    auto removeOffered = execute(connection,
        "DELETE FROM rosters WHERE league_id = $1 AND lower(manager_email) = lower($2) "
        "AND player_id IN (SELECT jsonb_array_elements_text($3::jsonb)) RETURNING player_id",
        {leagueId, trade.offeredBy, offeredIdsJson});
    auto removeRequested = execute(connection,
        "DELETE FROM rosters WHERE league_id = $1 AND lower(manager_email) = lower($2) "
        "AND player_id IN (SELECT jsonb_array_elements_text($3::jsonb)) RETURNING player_id",
        {leagueId, trade.offeredTo, requestedIdsJson});
    if (!tuplesOk(removeOffered)
        || static_cast<std::size_t>(PQntuples(removeOffered.get())) != trade.offeredPlayerIds.size()
        || !tuplesOk(removeRequested)
        || static_cast<std::size_t>(PQntuples(removeRequested.get())) != trade.requestedPlayerIds.size()) {
        result.errorCode = "trade_ownership_changed";
        return result;
    }

    for (const auto &[player, slot] : *offeredPlan) {
        const auto playerId = tradePlayerId(player);
        auto inserted = execute(connection,
            "INSERT INTO rosters "
            "(league_id, manager_email, player_id, player_snapshot, roster_slot, acquired_via, acquired_at) "
            "VALUES ($1, $2, $3, $4::jsonb, $5, 'trade', NOW()) RETURNING player_id",
            {leagueId, trade.offeredTo, playerId, jsonToString(player), slot});
        if (!tuplesOk(inserted) || PQntuples(inserted.get()) != 1) {
            result.errorCode = "trade_roster_conflict";
            return result;
        }
    }
    for (const auto &[player, slot] : *requestedPlan) {
        const auto playerId = tradePlayerId(player);
        auto inserted = execute(connection,
            "INSERT INTO rosters "
            "(league_id, manager_email, player_id, player_snapshot, roster_slot, acquired_via, acquired_at) "
            "VALUES ($1, $2, $3, $4::jsonb, $5, 'trade', NOW()) RETURNING player_id",
            {leagueId, trade.offeredBy, playerId, jsonToString(player), slot});
        if (!tuplesOk(inserted) || PQntuples(inserted.get()) != 1) {
            result.errorCode = "trade_roster_conflict";
            return result;
        }
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

drogon::HttpResponsePtr createTradeOfferPackage(const drogon::HttpRequestPtr &request,
                                                const std::string &leagueId,
                                                const std::string &email,
                                                bool legacy = false,
                                                bool counter = false) {
    const auto bodyPtr = request->getJsonObject();
    if (!bodyPtr || !bodyPtr->isObject()) {
        return errorResponse(drogon::k400BadRequest,
                             "Trade offer details are required.",
                             "trade_payload_required");
    }
    Json::Value body = *bodyPtr;
    const auto key = operationKey(request);
    const auto operationType = counter ? "counter" : "create";
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
        return tradeStateResponse(payload, false, payload.get("tradeId", "").asString(),
                                  legacy ? drogon::k201Created : drogon::k200OK);
    }
    if (!cff::trade_lifecycle::expectedVersionMatches(context->version, body, !legacy)) {
        const auto details = currentTradeStateDetailsPackage(
            context->connection.get(), leagueId, email, context->access, context->version);
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict,
                             "Trade state changed. Load the latest offers before retrying.",
                             "trade_state_conflict",
                             false,
                             details);
    }

    TradeRecordPackage counteredTrade;
    const auto counterTradeId = body.get("tradeId", body.get("counterTradeId", "")).asString();
    if (counter) {
        counteredTrade = tradeForUpdatePackage(context->connection.get(), leagueId, counterTradeId);
        if (!counteredTrade.found) {
            rollback(context->connection.get());
            return errorResponse(drogon::k404NotFound, "Trade offer not found.", "trade_not_found");
        }
        if (counteredTrade.status != "pending" || counteredTrade.offeredTo != email) {
            rollback(context->connection.get());
            return errorResponse(drogon::k409Conflict,
                                 "Only the receiving manager may counter a pending offer.",
                                 "trade_counter_not_allowed");
        }
        if (!body.isMember("offerPlayers") && !body.isMember("offerPlayer")) {
            body["offerPlayers"] = counteredTrade.requestedPlayers;
        }
        if (!body.isMember("requestPlayers") && !body.isMember("requestPlayer")) {
            body["requestPlayers"] = counteredTrade.offeredPlayers;
        }
        body["targetManager"] = counteredTrade.offeredBy;
    }

    const auto target = canonicalEmail(body.get(
        "targetManager", body.get("targetManagerEmail", "")).asString());
    if (target.empty() || target == email
        || !activeTradeMember(context->connection.get(), leagueId, target)) {
        rollback(context->connection.get());
        return errorResponse(drogon::k400BadRequest,
                             "Select a different active league manager for this trade.",
                             "invalid_trade_target");
    }

    const auto offeredRoster = rosterPayload(context->connection.get(), leagueId, email);
    const auto requestedRoster = rosterPayload(context->connection.get(), leagueId, target);
    Json::Value offeredPlayers;
    Json::Value requestedPlayers;
    std::vector<std::string> offeredPlayerIds;
    std::vector<std::string> requestedPlayerIds;
    if (!authoritativeTradePackage(
            tradePackageInput(body, "offerPlayers", "offerPlayer"),
            offeredRoster, offeredPlayers, offeredPlayerIds)) {
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict,
                             "Every offered player must still be on your roster.",
                             "offered_player_not_owned");
    }
    if (!authoritativeTradePackage(
            tradePackageInput(body, "requestPlayers", "requestPlayer"),
            requestedRoster, requestedPlayers, requestedPlayerIds)) {
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict,
                             "Every requested player must still be on the target manager's roster.",
                             "requested_player_not_owned");
    }
    if (!cff::trade_lifecycle::validOfferPlayerPackages(
            offeredPlayerIds, requestedPlayerIds, kMaximumTradePlayersPerSide)) {
        rollback(context->connection.get());
        return errorResponse(drogon::k400BadRequest,
                             "Each side must include one or more unique players.",
                             "invalid_trade_players");
    }

    if (counter && !releaseTradePlayerLocks(
            context->connection.get(), leagueId, counteredTrade.id)) {
        rollback(context->connection.get());
        return tradeStorageUnavailable();
    }
    for (const auto &playerId : offeredPlayerIds) {
        if (const auto locked = lockedTradeOffer(context->connection.get(), leagueId, playerId)) {
            Json::Value details(Json::objectValue);
            details["conflictingTradeId"] = *locked;
            rollback(context->connection.get());
            return errorResponse(drogon::k409Conflict,
                                 "An offered player is already locked in another open trade.",
                                 "trade_player_locked",
                                 false,
                                 details);
        }
    }
    for (const auto &playerId : requestedPlayerIds) {
        if (const auto locked = lockedTradeOffer(context->connection.get(), leagueId, playerId)) {
            Json::Value details(Json::objectValue);
            details["conflictingTradeId"] = *locked;
            rollback(context->connection.get());
            return errorResponse(drogon::k409Conflict,
                                 "A requested player is already locked in another open trade.",
                                 "trade_player_locked",
                                 false,
                                 details);
        }
    }

    const auto tradeId = tradeIdFor(leagueId, email, key);
    const auto requiresApproval = tradeApprovalRequired(context->access.tradeRules);
    const auto expirationHours = tradeExpirationHours(context->access.tradeRules);
    const auto note = body.get("note", "").asString().substr(0, 1000);
    const auto requestName = tradePackageNames(requestedPlayers);
    auto inserted = execute(context->connection.get(),
        "INSERT INTO trade_offers "
        "(id, league_id, offered_by_email, offered_to_email, offered_player_ids, requested_player_ids, "
        "offer_player_snapshot, request_player_snapshot, request_player_name, target_manager, note, "
        "requires_approval, status, expires_at, created_at, updated_at) "
        "VALUES ($1, $2, $3, $4, ARRAY(SELECT jsonb_array_elements_text($5::jsonb)), "
        "ARRAY(SELECT jsonb_array_elements_text($6::jsonb)), $7::jsonb, $8::jsonb, $9, $4, $10, "
        "$11::boolean, 'pending', NOW() + ($12::int * INTERVAL '1 hour'), NOW(), NOW()) RETURNING id",
        {tradeId, leagueId, email, target,
         jsonToString(idArray(offeredPlayerIds)), jsonToString(idArray(requestedPlayerIds)),
         jsonToString(offeredPlayers), jsonToString(requestedPlayers), requestName, note,
         requiresApproval ? "true" : "false", std::to_string(expirationHours)});
    if (!tuplesOk(inserted) || PQntuples(inserted.get()) != 1
        || !acquireTradePlayerLocksPackage(context->connection.get(), leagueId, tradeId,
                                           email, offeredPlayerIds, target, requestedPlayerIds)) {
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
    if (counter) {
        auto countered = execute(context->connection.get(),
            "UPDATE trade_offers SET status = 'declined', state_version = $3::bigint, "
            "resolution_reason = $4, resolved_at = COALESCE(resolved_at, NOW()), updated_at = NOW() "
            "WHERE league_id = $1 AND id = $2 AND status = 'pending' RETURNING id",
            {leagueId, counteredTrade.id, std::to_string(nextVersion), "countered:" + tradeId});
        if (!tuplesOk(countered) || PQntuples(countered.get()) != 1) {
            rollback(context->connection.get());
            return errorResponse(drogon::k409Conflict,
                                 "The original offer changed before the counter was saved.",
                                 "trade_transition_conflict");
        }
    }

    Json::Value metadata(Json::objectValue);
    metadata["tradeId"] = tradeId;
    metadata["offeredPlayerIds"] = idArray(offeredPlayerIds);
    metadata["requestedPlayerIds"] = idArray(requestedPlayerIds);
    metadata["targetManager"] = target;
    metadata["requiresApproval"] = requiresApproval;
    metadata["version"] = Json::Int64(nextVersion);
    if (counter) metadata["counteredTradeId"] = counteredTrade.id;
    const auto summary = (counter ? "Countered with " : "Offered ")
        + tradePackageNames(offeredPlayers) + " for " + tradePackageNames(requestedPlayers);
    if (!addTradeTransactionRecord(context->connection.get(), leagueId, email, key,
                                   counter ? "Trade Counter" : "Trade Offer",
                                   summary,
                                   metadata)) {
        rollback(context->connection.get());
        return tradeStorageUnavailable();
    }

    auto payload = tradeStatePayloadPackage(context->connection.get(), leagueId, email,
                                            context->access, nextVersion);
    payload["action"] = operationType;
    payload["tradeId"] = tradeId;
    payload["operationKey"] = key;
    if (counter) payload["counteredTradeId"] = counteredTrade.id;
    if (!recordTradeOperation(context->connection.get(), leagueId, email, key,
                              operationType, nextVersion, payload)) {
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

drogon::HttpResponsePtr updateTradeLifecycleStatusPackage(
    const drogon::HttpRequestPtr &request,
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
        const auto details = currentTradeStateDetailsPackage(
            context->connection.get(), leagueId, email, context->access, context->version);
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict,
                             "Trade state changed. Load the latest offers before retrying.",
                             "trade_state_conflict",
                             false,
                             details);
    }

    const auto trade = tradeForUpdatePackage(context->connection.get(), leagueId, tradeId);
    if (!trade.found) {
        rollback(context->connection.get());
        return errorResponse(drogon::k404NotFound, "Trade offer not found.", "trade_not_found");
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
        const auto details = currentTradeStateDetailsPackage(
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

    TradeSwapResultPackage swap;
    if (decision.execute) {
        swap = executeTradeSwapPackage(context->connection.get(), leagueId, trade,
                                       context->access.rosterRules);
        if (!swap.ok) {
            const auto details = currentTradeStateDetailsPackage(
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
    metadata["offeredPlayerIds"] = idArray(trade.offeredPlayerIds);
    metadata["requestedPlayerIds"] = idArray(trade.requestedPlayerIds);
    if (decision.execute) {
        metadata["offeredByRosterVersion"] = Json::Int64(swap.offeredByRosterVersion);
        metadata["offeredToRosterVersion"] = Json::Int64(swap.offeredToRosterVersion);
    }
    if (!addTradeTransactionRecord(context->connection.get(), leagueId, email, key,
                                   decision.execute ? "Trade Completed" : "Trade Status",
                                   decision.nextStatus + ": "
                                       + tradePackageNames(trade.offeredPlayers) + " for "
                                       + tradePackageNames(trade.requestedPlayers),
                                   metadata)) {
        rollback(context->connection.get());
        return tradeStorageUnavailable();
    }

    auto payload = tradeStatePayloadPackage(context->connection.get(), leagueId, email,
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

drogon::HttpResponsePtr dispatchTradeTransactionPackage(
    const drogon::HttpRequestPtr &request,
    const std::string &leagueId,
    const std::string &email) {
    const auto body = request->getJsonObject();
    if (!body || !body->isObject()) {
        return errorResponse(drogon::k400BadRequest,
                             "Trade transaction details are required.",
                             "trade_payload_required");
    }
    const auto action = lower(trim(body->get("action", "").asString()));
    if (action == "create") return createTradeOfferPackage(request, leagueId, email, false, false);
    if (action == "counter") return createTradeOfferPackage(request, leagueId, email, false, true);
    if (action == "status" || action == "accept" || action == "approve"
        || action == "decline" || action == "cancel" || action == "veto") {
        const auto tradeId = body->get("tradeId", "").asString();
        if (tradeId.empty()) {
            return errorResponse(drogon::k400BadRequest,
                                 "A trade ID is required.",
                                 "trade_id_required");
        }
        return updateTradeLifecycleStatusPackage(request, leagueId, email, tradeId, false);
    }
    return errorResponse(drogon::k400BadRequest,
                         "Trade action must be create, counter, or status.",
                         "invalid_trade_action");
}
