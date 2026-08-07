int lineupPositiveInt(const Json::Value &value) {
    const auto maximum = static_cast<Json::UInt64>(std::numeric_limits<int>::max());
    switch (value.type()) {
        case Json::intValue: {
            const auto parsed = value.asInt64();
            return parsed > 0 && static_cast<Json::UInt64>(parsed) <= maximum
                ? static_cast<int>(parsed)
                : 0;
        }
        case Json::uintValue: {
            const auto parsed = value.asUInt64();
            return parsed > 0 && parsed <= maximum ? static_cast<int>(parsed) : 0;
        }
        default:
            return 0;
    }
}

bool lineupWeekExists(PGconn *connection,
                      const std::string &leagueId,
                      int season,
                      int week) {
    auto result = execute(connection,
        "SELECT 1 FROM schedule_week_states "
        "WHERE league_id = $1 AND season = $2::int AND week = $3::int LIMIT 1",
        {leagueId, std::to_string(season), std::to_string(week)});
    return tuplesOk(result) && PQntuples(result.get()) == 1;
}

bool lineupWindowLocked(PGconn *connection,
                        const std::string &leagueId,
                        const std::string &email,
                        int season,
                        int week) {
    auto result = execute(connection,
        "SELECT EXISTS ("
        "SELECT 1 FROM schedule_week_states schedule "
        "WHERE schedule.league_id = $1 AND schedule.season = $3::int AND schedule.week = $4::int "
        "AND (schedule.status IN ('locked', 'finalized') "
        " OR (schedule.lineup_deadline IS NOT NULL AND schedule.lineup_deadline <= NOW())) "
        "UNION ALL SELECT 1 FROM lineup_week_states lineup "
        "WHERE lineup.league_id = $1 AND lower(lineup.manager_email) = lower($2) "
        "AND lineup.season = $3::int AND lineup.week = $4::int "
        "AND lineup.status IN ('locked', 'finalized') "
        "UNION ALL SELECT 1 FROM scoring_week_states scoring "
        "WHERE scoring.league_id = $1 AND scoring.season = $3::int AND scoring.week = $4::int "
        "AND scoring.status = 'final')",
        {leagueId, email, std::to_string(season), std::to_string(week)});
    return tuplesOk(result) && PQntuples(result.get()) > 0 && cell(result.get(), 0, 0) == "t";
}

std::string assignmentPlayerId(const Json::Value &assignment) {
    if (!assignment.isObject()) return "";
    const Json::Value *value = nullptr;
    if (assignment.isMember("playerId") && assignment["playerId"].isString()) {
        value = &assignment["playerId"];
    } else if (assignment.isMember("id") && assignment["id"].isString()) {
        value = &assignment["id"];
    }
    return value
        ? cff::roster_transaction::canonicalPlayerId(value->asString())
        : std::string{};
}

std::string assignmentSlot(const Json::Value &assignment) {
    if (!assignment.isObject()
        || !assignment.isMember("slot")
        || !assignment["slot"].isString()) {
        return "";
    }
    return lower(trim(assignment["slot"].asString()));
}

std::string rosterPlayerSlot(const Json::Value &player) {
    if (!player.isObject()
        || !player.isMember("rosterSlot")
        || !player["rosterSlot"].isString()) {
        return "bench";
    }
    auto slot = lower(trim(player["rosterSlot"].asString()));
    return slot.empty() ? "bench" : slot;
}

drogon::HttpResponsePtr mutateLineup(const drogon::HttpRequestPtr &request,
                                     const std::string &leagueId,
                                     const std::string &email) {
    const auto bodyPtr = request->getJsonObject();
    if (!bodyPtr || !bodyPtr->isObject()) {
        return errorResponse(drogon::k400BadRequest,
                             "A complete lineup payload is required.",
                             "lineup_payload_required");
    }
    const auto &body = *bodyPtr;
    const auto key = operationKey(request);
    if (key.empty()) {
        return errorResponse(drogon::k400BadRequest,
                             "An Idempotency-Key is required for lineup saves.",
                             "idempotency_key_required");
    }
    if (!body.isMember("assignments") || !body["assignments"].isArray()) {
        return errorResponse(drogon::k400BadRequest,
                             "A complete lineup assignment list is required.",
                             "lineup_assignments_required");
    }

    auto connection = connectDb();
    if (!connection || !begin(connection.get()) || !lockRosterLeague(connection.get(), leagueId)) {
        return errorResponse(drogon::k503ServiceUnavailable,
                             "Lineup storage is temporarily unavailable.",
                             "lineup_storage_unavailable",
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

    if (const auto replay = rosterOperationReplay(connection.get(), leagueId, email, key, "lineup")) {
        if (!(*replay)["operationTypeMatches"].asBool()) {
            Json::Value details(Json::objectValue);
            details["storedOperationType"] = (*replay)["storedOperationType"];
            rollback(connection.get());
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
                                 "Lineup replay could not be confirmed.",
                                 "lineup_confirmation_failed",
                                 true);
        }
        return rosterStateResponse(payload, false);
    }

    const auto currentVersion = rosterVersion(connection.get(), leagueId, email);
    if (!cff::roster_transaction::expectedVersionMatches(currentVersion, body, true)) {
        const auto details = currentStateDetails(connection.get(), leagueId, email, access);
        rollback(connection.get());
        return errorResponse(drogon::k409Conflict,
                             "Roster state changed. Reload the latest lineup before saving.",
                             "roster_state_conflict",
                             false,
                             details);
    }

    const auto season = lineupPositiveInt(body.get("season", 0));
    const auto week = lineupPositiveInt(body.get("week", 0));
    if (season <= 0 || week <= 0) {
        rollback(connection.get());
        return errorResponse(drogon::k400BadRequest,
                             "Season and week are required for lineup saves.",
                             "lineup_week_required");
    }
    if (!lineupWeekExists(connection.get(), leagueId, season, week)) {
        rollback(connection.get());
        return errorResponse(drogon::k409Conflict,
                             "The requested lineup week is not available in this league schedule.",
                             "lineup_week_unavailable");
    }
    if (lineupWindowLocked(connection.get(), leagueId, email, season, week)) {
        rollback(connection.get());
        return errorResponse(drogon::k409Conflict,
                             "This lineup is locked because the deadline passed or the week was locked.",
                             "lineup_locked");
    }

    const auto roster = rosterPayload(connection.get(), leagueId, email);
    const auto &assignments = body["assignments"];
    const auto validationErrors = cff::league_roster::lineupAssignmentErrors(
        roster, access.rosterRules, assignments);
    if (!validationErrors.empty()) {
        Json::Value details(Json::objectValue);
        details["errors"] = validationErrors;
        details["state"] = rosterStatePayload(connection.get(), leagueId, email, access, currentVersion);
        rollback(connection.get());
        return errorResponse(drogon::k409Conflict,
                             "The lineup is incomplete or contains invalid assignments.",
                             "invalid_lineup",
                             false,
                             details);
    }

    std::unordered_map<std::string, std::string> currentSlots;
    for (const auto &player : roster) {
        const auto playerId = player.isObject()
            && player.isMember("id")
            && player["id"].isString()
            ? cff::roster_transaction::canonicalPlayerId(player["id"].asString())
            : std::string{};
        if (!playerId.empty()) currentSlots[playerId] = rosterPlayerSlot(player);
    }

    Json::Value changedPlayers(Json::arrayValue);
    for (const auto &assignment : assignments) {
        const auto playerId = assignmentPlayerId(assignment);
        const auto slot = assignmentSlot(assignment);
        const auto current = currentSlots.find(playerId);
        if (current != currentSlots.end() && current->second == slot) continue;
        if (!cff::lineup_game_lock::slotMoveAllowed(
                connection.get(), leagueId, email, body, playerId)) {
            Json::Value details(Json::objectValue);
            details["playerId"] = playerId;
            rollback(connection.get());
            return errorResponse(drogon::k409Conflict,
                                 "A changed player is locked because that player's game has started.",
                                 "lineup_player_locked",
                                 false,
                                 details);
        }
        changedPlayers.append(playerId);
    }

    for (const auto &assignment : assignments) {
        const auto playerId = assignmentPlayerId(assignment);
        const auto slot = assignmentSlot(assignment);
        const auto current = currentSlots.find(playerId);
        if (current != currentSlots.end() && current->second == slot) continue;
        auto updated = execute(connection.get(),
            "UPDATE rosters SET roster_slot = $4 "
            "WHERE league_id = $1 AND lower(manager_email) = lower($2) AND player_id = $3 "
            "RETURNING player_id",
            {leagueId, email, playerId, slot});
        if (!tuplesOk(updated) || PQntuples(updated.get()) != 1) {
            rollback(connection.get());
            return errorResponse(drogon::k409Conflict,
                                 "The roster changed before the lineup save completed.",
                                 "lineup_save_conflict");
        }
    }

    const auto savedRoster = rosterPayload(connection.get(), leagueId, email);
    auto lineupState = execute(connection.get(),
        "INSERT INTO lineup_week_states ("
        "league_id, season, week, manager_email, version, status, lock_reason, "
        "lineup_snapshot, validation_errors, updated_at) "
        "VALUES ($1, $3::int, $4::int, $2, 1, 'open', '', $5::jsonb, '[]'::jsonb, NOW()) "
        "ON CONFLICT (league_id, season, week, manager_email) DO UPDATE SET "
        "version = lineup_week_states.version + 1, lineup_snapshot = EXCLUDED.lineup_snapshot, "
        "validation_errors = '[]'::jsonb, updated_at = NOW() "
        "WHERE lineup_week_states.status = 'open' RETURNING version",
        {leagueId, email, std::to_string(season), std::to_string(week), jsonToString(savedRoster)});
    if (!tuplesOk(lineupState) || PQntuples(lineupState.get()) != 1) {
        rollback(connection.get());
        return errorResponse(drogon::k409Conflict,
                             "The lineup locked before the save completed.",
                             "lineup_locked");
    }
    const auto lineupVersion = cellInt64(lineupState.get(), 0, 0, 0);

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
    metadata["lineupVersion"] = Json::Int64(lineupVersion);
    metadata["season"] = season;
    metadata["week"] = week;
    metadata["changedPlayerIds"] = changedPlayers;
    const auto changedCount = static_cast<int>(changedPlayers.size());
    const auto summary = "Saved week " + std::to_string(week) + " lineup ("
        + std::to_string(changedCount) + " changed player" + (changedCount == 1 ? "" : "s") + ")";
    if (!addTransactionRecord(connection.get(), leagueId, email, key, "Lineup", summary, metadata)) {
        rollback(connection.get());
        return errorResponse(drogon::k503ServiceUnavailable,
                             "Lineup history could not be recorded.",
                             "lineup_transaction_unavailable",
                             true);
    }

    auto payload = rosterStatePayload(connection.get(), leagueId, email, access, nextVersion);
    payload["action"] = "lineup";
    payload["operationKey"] = key;
    payload["message"] = summary;
    payload["season"] = season;
    payload["week"] = week;
    payload["lineupVersion"] = Json::Int64(lineupVersion);
    payload["changedCount"] = changedCount;
    payload["changedPlayerIds"] = changedPlayers;

    if (!recordRosterOperation(
            connection.get(), leagueId, email, key, "lineup", nextVersion, payload)) {
        rollback(connection.get());
        return errorResponse(drogon::k503ServiceUnavailable,
                             "Lineup replay protection could not be recorded.",
                             "roster_operation_unavailable",
                             true);
    }
    if (!commit(connection.get())) {
        rollback(connection.get());
        return errorResponse(drogon::k503ServiceUnavailable,
                             "Lineup save could not be confirmed.",
                             "lineup_confirmation_failed",
                             true);
    }
    return rosterStateResponse(payload, false);
}
