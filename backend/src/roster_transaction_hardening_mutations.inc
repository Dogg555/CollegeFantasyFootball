enum class RosterAction { Add, Drop, Swap, Slot };

std::string rosterActionName(RosterAction action) {
    switch (action) {
        case RosterAction::Add: return "add";
        case RosterAction::Drop: return "drop";
        case RosterAction::Swap: return "swap";
        case RosterAction::Slot: return "slot";
    }
    return "unknown";
}

std::string playerNameFromRoster(const Json::Value &roster,
                                 const std::string &playerId) {
    for (const auto &player : roster) {
        if (cff::roster_transaction::canonicalPlayerId(player.get("id", "").asString()) == playerId) {
            return player.get("name", playerId).asString();
        }
    }
    return playerId;
}

drogon::HttpResponsePtr getRosterState(const std::string &leagueId,
                                       const std::string &email) {
    auto connection = connectDb();
    if (!connection || !begin(connection.get()) || !lockRosterLeague(connection.get(), leagueId)) {
        return errorResponse(drogon::k503ServiceUnavailable,
                             "Roster storage is temporarily unavailable.",
                             "roster_storage_unavailable",
                             true);
    }
    const auto access = leagueAccess(connection.get(), leagueId, email);
    if (!access.exists) {
        rollback(connection.get());
        return errorResponse(drogon::k404NotFound, "League not found.", "league_not_found");
    }
    if (!access.member) {
        rollback(connection.get());
        return errorResponse(drogon::k403Forbidden,
                             "Active league membership is required.",
                             "league_membership_required");
    }
    if (!ensureRosterState(connection.get(), leagueId, email)) {
        rollback(connection.get());
        return errorResponse(drogon::k503ServiceUnavailable,
                             "Roster state could not be initialized.",
                             "roster_state_unavailable",
                             true);
    }
    auto payload = rosterStatePayload(connection.get(), leagueId, email, access);
    if (!commit(connection.get())) {
        rollback(connection.get());
        return errorResponse(drogon::k503ServiceUnavailable,
                             "Roster state could not be confirmed.",
                             "roster_confirmation_failed",
                             true);
    }
    return jsonResponse(payload);
}

drogon::HttpResponsePtr mutateRoster(const drogon::HttpRequestPtr &request,
                                     const std::string &leagueId,
                                     const std::string &email,
                                     RosterAction action,
                                     const std::string &pathPlayerId = "",
                                     bool legacyArray = false) {
    const auto bodyPtr = request->getJsonObject();
    if (!bodyPtr || !bodyPtr->isObject()) {
        return errorResponse(drogon::k400BadRequest,
                             "Roster transaction details are required.",
                             "roster_payload_required");
    }
    const auto &body = *bodyPtr;
    const auto actionName = rosterActionName(action);
    const auto key = operationKey(request);
    if (!legacyArray && key.empty()) {
        return errorResponse(drogon::k400BadRequest,
                             "An Idempotency-Key is required for roster transactions.",
                             "idempotency_key_required");
    }

    auto connection = connectDb();
    if (!connection || !begin(connection.get()) || !lockRosterLeague(connection.get(), leagueId)) {
        return errorResponse(drogon::k503ServiceUnavailable,
                             "Roster storage is temporarily unavailable.",
                             "roster_storage_unavailable",
                             true);
    }

    const auto access = leagueAccess(connection.get(), leagueId, email);
    if (!access.exists) {
        rollback(connection.get());
        return errorResponse(drogon::k404NotFound, "League not found.", "league_not_found");
    }
    if (!access.member) {
        rollback(connection.get());
        return errorResponse(drogon::k403Forbidden,
                             "Active league membership is required.",
                             "league_membership_required");
    }
    if (!ensureRosterState(connection.get(), leagueId, email)) {
        rollback(connection.get());
        return errorResponse(drogon::k503ServiceUnavailable,
                             "Roster state could not be initialized.",
                             "roster_state_unavailable",
                             true);
    }

    if (const auto replay = rosterOperationReplay(
            connection.get(), leagueId, email, key, actionName)) {
        if (!(*replay)["operationTypeMatches"].asBool()) {
            rollback(connection.get());
            Json::Value details(Json::objectValue);
            details["storedOperationType"] = (*replay)["storedOperationType"];
            return errorResponse(drogon::k409Conflict,
                                 "This idempotency key was already used for another roster action.",
                                 "idempotency_key_conflict",
                                 false,
                                 details);
        }
        auto payload = *replay;
        payload.removeMember("operationTypeMatches");
        payload.removeMember("storedOperationType");
        if (!commit(connection.get())) {
            rollback(connection.get());
            return errorResponse(drogon::k503ServiceUnavailable,
                                 "Roster replay could not be confirmed.",
                                 "roster_confirmation_failed",
                                 true);
        }
        return rosterStateResponse(payload, legacyArray);
    }

    const auto currentVersion = rosterVersion(connection.get(), leagueId, email);
    if (!cff::roster_transaction::expectedVersionMatches(currentVersion, body, !legacyArray)) {
        const auto details = currentStateDetails(connection.get(), leagueId, email, access);
        rollback(connection.get());
        return errorResponse(drogon::k409Conflict,
                             "Roster state changed. Reload the latest roster before retrying.",
                             "roster_state_conflict",
                             false,
                             details);
    }

    if (lineupLocked(connection.get(), leagueId)) {
        rollback(connection.get());
        return errorResponse(drogon::k409Conflict,
                             "Roster changes are locked after a matchup is finalized.",
                             "lineup_locked");
    }

    if ((action == RosterAction::Add || action == RosterAction::Swap)
        && !cff::roster_transaction::directAcquisitionAllowed(access.waiverRules)) {
        Json::Value details(Json::objectValue);
        details["waiverMode"] = access.waiverRules.get("mode", "waivers").asString();
        details["claimEndpoint"] = "/api/leagues/" + leagueId + "/waivers";
        details["state"] = rosterStatePayload(connection.get(), leagueId, email, access, currentVersion);
        rollback(connection.get());
        return errorResponse(drogon::k409Conflict,
                             "This league requires a waiver claim before adding that player.",
                             "waiver_claim_required",
                             false,
                             details);
    }

    const auto roster = rosterPayload(connection.get(), leagueId, email);
    Json::Value addPlayer(Json::objectValue);
    if (body.isMember("addPlayer") && body["addPlayer"].isObject()) addPlayer = body["addPlayer"];
    else if (body.isMember("player") && body["player"].isObject()) addPlayer = body["player"];

    auto addPlayerId = cff::roster_transaction::canonicalPlayerId(addPlayer.get("id", "").asString());
    if (addPlayerId.empty()) {
        addPlayerId = cff::roster_transaction::canonicalPlayerId(addPlayer.get("playerId", "").asString());
    }
    auto dropPlayerId = cff::roster_transaction::canonicalPlayerId(
        body.get("dropPlayerId", body.get("playerId", "")).asString());
    auto slotPlayerId = cff::roster_transaction::canonicalPlayerId(
        pathPlayerId.empty() ? body.get("playerId", "").asString() : pathPlayerId);
    const auto requestedSlot = lower(trim(body.get("slot", "").asString()));

    if ((action == RosterAction::Add || action == RosterAction::Swap) && addPlayerId.empty()) {
        rollback(connection.get());
        return errorResponse(drogon::k400BadRequest,
                             "The player being added must include an ID.",
                             "player_id_required");
    }
    if ((action == RosterAction::Drop || action == RosterAction::Swap) && dropPlayerId.empty()) {
        rollback(connection.get());
        return errorResponse(drogon::k400BadRequest,
                             "The player being dropped must include an ID.",
                             "drop_player_required");
    }
    if (action == RosterAction::Slot && (slotPlayerId.empty() || requestedSlot.empty())) {
        rollback(connection.get());
        return errorResponse(drogon::k400BadRequest,
                             "Player and roster slot are required.",
                             "roster_slot_required");
    }
    if (action == RosterAction::Swap && addPlayerId == dropPlayerId) {
        rollback(connection.get());
        return errorResponse(drogon::k400BadRequest,
                             "The same player cannot be added and dropped.",
                             "invalid_roster_swap");
    }

    if (action == RosterAction::Drop || action == RosterAction::Swap) {
        if (!cff::roster_transaction::rosterContainsPlayer(roster, dropPlayerId)) {
            rollback(connection.get());
            return errorResponse(drogon::k409Conflict,
                                 "The selected drop player is no longer on your roster.",
                                 "drop_player_not_rostered");
        }
    }

    if (action == RosterAction::Slot) {
        Json::Value selectedPlayer(Json::objectValue);
        for (const auto &player : roster) {
            if (cff::roster_transaction::canonicalPlayerId(player.get("id", "").asString()) == slotPlayerId) {
                selectedPlayer = player;
                break;
            }
        }
        if (!selectedPlayer.isObject() || selectedPlayer.empty()) {
            rollback(connection.get());
            return errorResponse(drogon::k404NotFound,
                                 "Roster player not found.",
                                 "roster_player_not_found");
        }
        if (!cff::league_roster::validateRosterSlotMove(
                selectedPlayer, roster, access.rosterRules, slotPlayerId, requestedSlot)) {
            rollback(connection.get());
            return errorResponse(drogon::k409Conflict,
                                 "That player is not eligible for the requested roster slot or the slot is full.",
                                 "invalid_roster_slot");
        }
    }

    std::optional<std::string> destination;
    if (action == RosterAction::Add || action == RosterAction::Swap) {
        if (const auto owner = playerOwner(connection.get(), leagueId, addPlayerId)) {
            rollback(connection.get());
            return errorResponse(drogon::k409Conflict,
                                 *owner == email
                                     ? "That player is already on your roster."
                                     : "That player is already rostered by another manager.",
                                 *owner == email ? "player_already_rostered" : "player_unavailable");
        }
        destination = cff::roster_transaction::destinationSlot(
            addPlayer,
            roster,
            access.rosterRules,
            action == RosterAction::Swap ? dropPlayerId : "");
        if (!destination) {
            Json::Value details(Json::objectValue);
            details["rosterCount"] = static_cast<Json::UInt64>(roster.size());
            details["rosterLimit"] = cff::league_roster::rosterLimitFromRules(access.rosterRules);
            rollback(connection.get());
            return errorResponse(drogon::k409Conflict,
                                 "Your roster is full. Select a player to drop in the same transaction.",
                                 "roster_full",
                                 false,
                                 details);
        }
    }

    const auto droppedName = dropPlayerId.empty()
        ? std::string{}
        : playerNameFromRoster(roster, dropPlayerId);
    const auto addedName = addPlayer.get("name", addPlayerId).asString();

    if (action == RosterAction::Drop || action == RosterAction::Swap) {
        auto removed = execute(connection.get(),
            "DELETE FROM rosters WHERE league_id = $1 AND lower(manager_email) = lower($2) "
            "AND player_id = $3 RETURNING player_id",
            {leagueId, email, dropPlayerId});
        if (!tuplesOk(removed) || PQntuples(removed.get()) != 1) {
            rollback(connection.get());
            return errorResponse(drogon::k409Conflict,
                                 "The selected drop player changed before the transaction completed.",
                                 "drop_player_conflict");
        }
    }

    if (action == RosterAction::Add || action == RosterAction::Swap) {
        addPlayer["id"] = addPlayerId;
        addPlayer["playerId"] = addPlayerId;
        auto inserted = execute(connection.get(),
            "INSERT INTO rosters "
            "(league_id, manager_email, player_id, player_snapshot, roster_slot, acquired_via, acquired_at) "
            "VALUES ($1, $2, $3, $4::jsonb, $5, 'free_agent', NOW()) RETURNING player_id",
            {leagueId, email, addPlayerId, jsonToString(addPlayer), *destination});
        if (!tuplesOk(inserted) || PQntuples(inserted.get()) != 1) {
            rollback(connection.get());
            return errorResponse(drogon::k409Conflict,
                                 "That player became unavailable before the transaction completed.",
                                 "player_unavailable");
        }
    }

    if (action == RosterAction::Slot) {
        auto updated = execute(connection.get(),
            "UPDATE rosters SET roster_slot = $4 "
            "WHERE league_id = $1 AND lower(manager_email) = lower($2) AND player_id = $3 "
            "RETURNING player_id",
            {leagueId, email, slotPlayerId, requestedSlot});
        if (!tuplesOk(updated) || PQntuples(updated.get()) != 1) {
            rollback(connection.get());
            return errorResponse(drogon::k409Conflict,
                                 "The roster player changed before the slot update completed.",
                                 "roster_slot_conflict");
        }
    }

    const auto nextVersion = advanceRosterVersion(connection.get(), leagueId, email);
    if (nextVersion < 0) {
        rollback(connection.get());
        return errorResponse(drogon::k503ServiceUnavailable,
                             "Roster revision could not be advanced.",
                             "roster_state_unavailable",
                             true);
    }

    Json::Value metadata(Json::objectValue);
    metadata["operationKey"] = key;
    metadata["version"] = Json::Int64(nextVersion);
    if (!addPlayerId.empty()) metadata["addedPlayerId"] = addPlayerId;
    if (!dropPlayerId.empty()) metadata["droppedPlayerId"] = dropPlayerId;
    if (!slotPlayerId.empty()) metadata["playerId"] = slotPlayerId;
    if (!requestedSlot.empty()) metadata["rosterSlot"] = requestedSlot;

    std::string transactionType;
    std::string summary;
    if (action == RosterAction::Add) {
        transactionType = "Free Agent";
        summary = "Added " + addedName;
    } else if (action == RosterAction::Drop) {
        transactionType = "Drop";
        summary = "Dropped " + droppedName;
    } else if (action == RosterAction::Swap) {
        transactionType = "Add/Drop";
        summary = "Added " + addedName + ", dropped " + droppedName;
    } else {
        transactionType = "Lineup";
        summary = "Moved " + playerNameFromRoster(roster, slotPlayerId) + " to " + requestedSlot;
    }
    if (!addTransactionRecord(connection.get(), leagueId, email, key, transactionType, summary, metadata)) {
        rollback(connection.get());
        return errorResponse(drogon::k503ServiceUnavailable,
                             "Roster transaction history could not be recorded.",
                             "roster_transaction_unavailable",
                             true);
    }

    auto payload = rosterStatePayload(connection.get(), leagueId, email, access, nextVersion);
    payload["action"] = actionName;
    payload["operationKey"] = key;
    payload["message"] = summary;
    if (!addPlayerId.empty()) payload["addedPlayerId"] = addPlayerId;
    if (!dropPlayerId.empty()) payload["droppedPlayerId"] = dropPlayerId;
    if (!slotPlayerId.empty()) payload["playerId"] = slotPlayerId;
    if (!requestedSlot.empty()) payload["rosterSlot"] = requestedSlot;

    if (!recordRosterOperation(
            connection.get(), leagueId, email, key, actionName, nextVersion, payload)) {
        rollback(connection.get());
        return errorResponse(drogon::k503ServiceUnavailable,
                             "Roster replay protection could not be recorded.",
                             "roster_operation_unavailable",
                             true);
    }
    if (!commit(connection.get())) {
        rollback(connection.get());
        return errorResponse(drogon::k503ServiceUnavailable,
                             "Roster transaction could not be confirmed.",
                             "roster_confirmation_failed",
                             true);
    }
    return rosterStateResponse(payload, legacyArray);
}

drogon::HttpResponsePtr dispatchRosterTransaction(const drogon::HttpRequestPtr &request,
                                                  const std::string &leagueId,
                                                  const std::string &email) {
    const auto body = request->getJsonObject();
    if (!body || !body->isObject()) {
        return errorResponse(drogon::k400BadRequest,
                             "Roster transaction details are required.",
                             "roster_payload_required");
    }
    const auto action = lower(trim(body->get("action", "").asString()));
    if (action == "add") return mutateRoster(request, leagueId, email, RosterAction::Add);
    if (action == "drop") return mutateRoster(request, leagueId, email, RosterAction::Drop);
    if (action == "swap") return mutateRoster(request, leagueId, email, RosterAction::Swap);
    if (action == "slot") return mutateRoster(request, leagueId, email, RosterAction::Slot);
    return errorResponse(drogon::k400BadRequest,
                         "Roster action must be add, drop, swap, or slot.",
                         "invalid_roster_action");
}
