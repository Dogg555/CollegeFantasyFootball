drogon::HttpResponsePtr undoPick(const drogon::HttpRequestPtr &request,
                                 const std::string &leagueId,
                                 const std::string &email) {
    auto connection = connectDb();
    if (!connection || !begin(connection.get()) || !lockDraft(connection.get(), leagueId)) return unavailable();
    const auto access = leagueAccess(connection.get(), leagueId, email);
    if (auto response = requireAccessResponse(access)) {
        rollback(connection.get());
        return response;
    }
    if (!access.commissioner) {
        rollback(connection.get());
        return errorResponse(drogon::k403Forbidden, "Only the commissioner can undo a pick.", "commissioner_required");
    }
    const auto key = operationKey(request);
    if (const auto replay = operationReplay(connection.get(), leagueId, key)) {
        if (*replay != "undo") {
            rollback(connection.get());
            return errorResponse(drogon::k409Conflict, "This operation key was already used.", "draft_operation_key_conflict");
        }
        if (!commit(connection.get())) return unavailable();
        auto payload = draftPayload(connection.get(), leagueId, email);
        payload["idempotentReplay"] = true;
        return jsonResponse(payload);
    }
    auto state = execute(connection.get(),
        "SELECT version, current_pick, status, to_json(draft_order)::text "
        "FROM draft_states WHERE league_id = $1 FOR UPDATE",
        {leagueId});
    if (!tuplesOk(state) || PQntuples(state.get()) == 0) {
        rollback(connection.get());
        return unavailable();
    }
    const auto versionBeforeUndo = cellInt64(state.get(), 0, 0, 0);
    const int currentPickBeforeUndo = cellInt(state.get(), 0, 1, 1);
    const auto orderBeforeUndo = jsonFromString(cell(state.get(), 0, 3), Json::Value{Json::arrayValue});
    const auto currentManagerBeforeUndo = cff::draft_lifecycle::managerForPick(
        orderBeforeUndo, currentPickBeforeUndo, access.draftType);
    if (auto response = requireExpectedVersion(
            request, versionBeforeUndo, currentPickBeforeUndo, currentManagerBeforeUndo)) {
        rollback(connection.get());
        return response;
    }
    auto last = execute(connection.get(),
        "SELECT pick_number, lower(manager_email), player_id FROM draft_picks "
        "WHERE league_id = $1 ORDER BY pick_number DESC LIMIT 1 FOR UPDATE",
        {leagueId});
    if (!tuplesOk(last)) {
        rollback(connection.get());
        return unavailable();
    }
    if (PQntuples(last.get()) == 0) {
        rollback(connection.get());
        return errorResponse(drogon::k409Conflict, "There is no draft pick to undo.", "draft_no_pick_to_undo");
    }
    const int pickNumber = cellInt(last.get(), 0, 0, 1);
    const auto manager = cell(last.get(), 0, 1);
    const auto playerId = cell(last.get(), 0, 2);
    if (!commandOk(execute(connection.get(),
        "DELETE FROM rosters WHERE league_id = $1 AND lower(manager_email) = lower($2) "
        "AND player_id = $3 AND acquired_via = 'draft'",
        {leagueId, manager, playerId}))
        || !commandOk(execute(connection.get(),
        "DELETE FROM draft_picks WHERE league_id = $1 AND pick_number = $2::integer",
        {leagueId, std::to_string(pickNumber)}))) {
        rollback(connection.get());
        return unavailable();
    }
    auto updated = execute(connection.get(),
        "UPDATE draft_states SET status = 'open', current_pick = $2::integer, completed_at = NULL, "
        "pick_deadline = NOW() + (pick_clock_seconds * INTERVAL '1 second'), "
        "version = version + 1, updated_at = NOW() WHERE league_id = $1 RETURNING version",
        {leagueId, std::to_string(pickNumber)});
    if (!tuplesOk(updated) || PQntuples(updated.get()) == 0) {
        rollback(connection.get());
        return unavailable();
    }
    const auto version = cellInt64(updated.get(), 0, 0, 0);
    if (!recordOperation(connection.get(), leagueId, key, "undo", version) || !commit(connection.get())) {
        rollback(connection.get());
        return unavailable();
    }
    auto payload = draftPayload(connection.get(), leagueId, email);
    payload["undonePick"] = pickNumber;
    payload["undonePlayerId"] = playerId;
    return jsonResponse(payload);
}

drogon::HttpResponsePtr resetDraft(const drogon::HttpRequestPtr &request,
                                    const std::string &leagueId,
                                    const std::string &email) {
    auto connection = connectDb();
    if (!connection || !begin(connection.get()) || !lockDraft(connection.get(), leagueId)) return unavailable();
    const auto access = leagueAccess(connection.get(), leagueId, email);
    if (auto response = requireAccessResponse(access)) {
        rollback(connection.get());
        return response;
    }
    if (!access.commissioner) {
        rollback(connection.get());
        return errorResponse(drogon::k403Forbidden, "Only the commissioner can reset the draft.", "commissioner_required");
    }
    const auto key = operationKey(request);
    if (const auto replay = operationReplay(connection.get(), leagueId, key)) {
        if (*replay != "reset") {
            rollback(connection.get());
            return errorResponse(drogon::k409Conflict, "This operation key was already used.", "draft_operation_key_conflict");
        }
        if (!commit(connection.get())) return unavailable();
        auto payload = draftPayload(connection.get(), leagueId, email);
        payload["idempotentReplay"] = true;
        return jsonResponse(payload);
    }
    const auto managers = activeManagers(connection.get(), leagueId);
    (void)ensureDraftState(connection.get(), leagueId, managers);
    auto state = execute(connection.get(),
        "SELECT version, current_pick, to_json(draft_order)::text "
        "FROM draft_states WHERE league_id = $1 FOR UPDATE",
        {leagueId});
    if (!tuplesOk(state) || PQntuples(state.get()) == 0) {
        rollback(connection.get());
        return unavailable();
    }
    const auto versionBeforeReset = cellInt64(state.get(), 0, 0, 0);
    const int currentPickBeforeReset = cellInt(state.get(), 0, 1, 1);
    const auto orderBeforeReset = jsonFromString(cell(state.get(), 0, 2), Json::Value{Json::arrayValue});
    const auto currentManagerBeforeReset = cff::draft_lifecycle::managerForPick(
        orderBeforeReset, currentPickBeforeReset, access.draftType);
    if (auto response = requireExpectedVersion(
            request, versionBeforeReset, currentPickBeforeReset, currentManagerBeforeReset)) {
        rollback(connection.get());
        return response;
    }
    if (!commandOk(execute(connection.get(),
        "DELETE FROM rosters WHERE league_id = $1 AND acquired_via = 'draft'",
        {leagueId}))
        || !commandOk(execute(connection.get(),
        "DELETE FROM draft_picks WHERE league_id = $1",
        {leagueId}))
        || !commandOk(execute(connection.get(),
        "UPDATE draft_readiness SET ready = FALSE, updated_at = NOW() WHERE league_id = $1",
        {leagueId}))) {
        rollback(connection.get());
        return unavailable();
    }
    const auto order = cff::draft_lifecycle::canonicalOrder(managers);
    auto updated = execute(connection.get(),
        "UPDATE draft_states SET status = 'not_started', current_pick = 1, "
        "draft_order = ARRAY(SELECT jsonb_array_elements_text($2::jsonb)), "
        "pick_deadline = NULL, started_at = NULL, completed_at = NULL, "
        "version = version + 1, updated_at = NOW() WHERE league_id = $1 RETURNING version",
        {leagueId, jsonToString(order)});
    if (!tuplesOk(updated) || PQntuples(updated.get()) == 0) {
        rollback(connection.get());
        return unavailable();
    }
    const auto version = cellInt64(updated.get(), 0, 0, 0);
    if (!recordOperation(connection.get(), leagueId, key, "reset", version) || !commit(connection.get())) {
        rollback(connection.get());
        return unavailable();
    }
    return jsonResponse(draftPayload(connection.get(), leagueId, email));
}
