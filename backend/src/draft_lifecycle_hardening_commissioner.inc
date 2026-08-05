drogon::HttpResponsePtr saveOrder(const drogon::HttpRequestPtr &request,
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
        return errorResponse(drogon::k403Forbidden, "Only the commissioner can set draft order.", "commissioner_required");
    }
    const auto key = operationKey(request);
    if (const auto replay = operationReplay(connection.get(), leagueId, key)) {
        if (*replay != "order") {
            rollback(connection.get());
            return errorResponse(drogon::k409Conflict, "This operation key was already used.", "draft_operation_key_conflict");
        }
        if (!commit(connection.get())) return unavailable();
        auto payload = draftPayload(connection.get(), leagueId, email);
        payload["idempotentReplay"] = true;
        return jsonResponse(payload);
    }
    const auto body = request->getJsonObject();
    if (!body || !body->isObject() || !body->isMember("draftOrder")) {
        rollback(connection.get());
        return errorResponse(drogon::k400BadRequest, "Draft order is required.", "draft_order_required");
    }
    const auto managers = activeManagers(connection.get(), leagueId);
    const auto order = (*body)["draftOrder"];
    if (!cff::draft_lifecycle::orderMatchesManagers(order, managers)) {
        rollback(connection.get());
        return errorResponse(drogon::k409Conflict,
                             "Draft order must contain each active manager exactly once.",
                             "draft_order_mismatch");
    }
    (void)ensureDraftState(connection.get(), leagueId, managers);
    auto state = execute(connection.get(),
        "SELECT status, version, (SELECT COUNT(*) FROM draft_picks WHERE league_id = $1) "
        "FROM draft_states WHERE league_id = $1 FOR UPDATE",
        {leagueId});
    if (!tuplesOk(state) || PQntuples(state.get()) == 0) {
        rollback(connection.get());
        return unavailable();
    }
    const auto currentVersion = cellInt64(state.get(), 0, 1, 0);
    if (auto response = requireExpectedVersion(request, currentVersion)) {
        rollback(connection.get());
        return response;
    }
    if (cell(state.get(), 0, 0) != "not_started" || cellInt(state.get(), 0, 2, 0) > 0) {
        rollback(connection.get());
        return errorResponse(drogon::k409Conflict,
                             "Draft order is locked after the draft starts.",
                             "draft_order_locked");
    }
    auto updated = execute(connection.get(),
        "UPDATE draft_states SET draft_order = ARRAY(SELECT jsonb_array_elements_text($2::jsonb)), "
        "version = version + 1, updated_at = NOW() WHERE league_id = $1 RETURNING version",
        {leagueId, jsonToString(order)});
    if (!tuplesOk(updated) || PQntuples(updated.get()) == 0) {
        rollback(connection.get());
        return unavailable();
    }
    const auto version = cellInt64(updated.get(), 0, 0, 0);
    if (!recordOperation(connection.get(), leagueId, key, "order", version) || !commit(connection.get())) {
        rollback(connection.get());
        return unavailable();
    }
    return jsonResponse(draftPayload(connection.get(), leagueId, email));
}

drogon::HttpResponsePtr startDraft(const drogon::HttpRequestPtr &request,
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
        return errorResponse(drogon::k403Forbidden, "Only the commissioner can start the draft.", "commissioner_required");
    }
    if (!access.lobbyOpen) {
        rollback(connection.get());
        return errorResponse(drogon::k409Conflict, "Open the draft lobby before starting.", "draft_lobby_closed");
    }
    const auto key = operationKey(request);
    if (const auto replay = operationReplay(connection.get(), leagueId, key)) {
        if (*replay != "start") {
            rollback(connection.get());
            return errorResponse(drogon::k409Conflict, "This operation key was already used.", "draft_operation_key_conflict");
        }
        if (!commit(connection.get())) return unavailable();
        auto payload = draftPayload(connection.get(), leagueId, email);
        payload["idempotentReplay"] = true;
        return jsonResponse(payload);
    }
    const auto managers = activeManagers(connection.get(), leagueId);
    if (managers.size() < 2) {
        rollback(connection.get());
        return errorResponse(drogon::k409Conflict,
                             "At least two active managers are required.",
                             "draft_manager_count_too_low");
    }
    const auto readiness = readinessPayload(connection.get(), leagueId);
    const auto body = request->getJsonObject();
    const bool force = body && body->isObject() && body->get("force", false).asBool();
    if (!force && !cff::draft_lifecycle::allManagersReady(managers, readiness)) {
        rollback(connection.get());
        Json::Value details(Json::objectValue);
        details["readiness"] = readiness;
        return errorResponse(drogon::k409Conflict,
                             "Every active manager must be ready before the draft starts.",
                             "draft_managers_not_ready",
                             false,
                             details);
    }
    (void)ensureDraftState(connection.get(), leagueId, managers);
    auto state = execute(connection.get(),
        "SELECT status, to_json(draft_order)::text, pick_clock_seconds, version, "
        "(SELECT COUNT(*) FROM draft_picks WHERE league_id = $1) "
        "FROM draft_states WHERE league_id = $1 FOR UPDATE",
        {leagueId});
    if (!tuplesOk(state) || PQntuples(state.get()) == 0) {
        rollback(connection.get());
        return unavailable();
    }
    const auto currentStatus = cell(state.get(), 0, 0);
    const auto currentVersion = cellInt64(state.get(), 0, 3, 0);
    if (auto response = requireExpectedVersion(request, currentVersion)) {
        rollback(connection.get());
        return response;
    }
    if (currentStatus == "open") {
        if (!recordOperation(connection.get(), leagueId, key, "start", cellInt64(state.get(), 0, 3, 0))
            || !commit(connection.get())) {
            rollback(connection.get());
            return unavailable();
        }
        auto payload = draftPayload(connection.get(), leagueId, email);
        payload["idempotentReplay"] = true;
        return jsonResponse(payload);
    }
    if (currentStatus == "complete" || cellInt(state.get(), 0, 4, 0) > 0) {
        rollback(connection.get());
        return errorResponse(drogon::k409Conflict,
                             "Reset the completed draft before starting again.",
                             "draft_reset_required");
    }
    auto order = jsonFromString(cell(state.get(), 0, 1), Json::Value{Json::arrayValue});
    if (!cff::draft_lifecycle::orderMatchesManagers(order, managers)) {
        order = cff::draft_lifecycle::canonicalOrder(managers);
    }
    auto updated = execute(connection.get(),
        "UPDATE draft_states SET status = 'open', current_pick = 1, "
        "draft_order = ARRAY(SELECT jsonb_array_elements_text($2::jsonb)), "
        "pick_deadline = NOW() + (pick_clock_seconds * INTERVAL '1 second'), "
        "started_at = NOW(), completed_at = NULL, version = version + 1, updated_at = NOW() "
        "WHERE league_id = $1 RETURNING version",
        {leagueId, jsonToString(order)});
    if (!tuplesOk(updated) || PQntuples(updated.get()) == 0) {
        rollback(connection.get());
        return unavailable();
    }
    const auto version = cellInt64(updated.get(), 0, 0, 0);
    (void)logDraftActivity(connection.get(), leagueId, email, "draft_started", "Draft started by commissioner.");
    if (!recordOperation(connection.get(), leagueId, key, "start", version) || !commit(connection.get())) {
        rollback(connection.get());
        return unavailable();
    }
    return jsonResponse(draftPayload(connection.get(), leagueId, email));
}

std::unordered_map<std::string, int> rosterCounts(PGconn *connection,
                                                   const std::string &leagueId,
                                                   const std::string &email) {
    std::unordered_map<std::string, int> counts;
    auto result = execute(connection,
        "SELECT roster_slot, COUNT(*) FROM rosters "
        "WHERE league_id = $1 AND lower(manager_email) = lower($2) GROUP BY roster_slot",
        {leagueId, email});
    if (!tuplesOk(result)) return counts;
    for (int row = 0; row < PQntuples(result.get()); ++row) {
        counts[cell(result.get(), row, 0)] = cellInt(result.get(), row, 1, 0);
    }
    return counts;
}
