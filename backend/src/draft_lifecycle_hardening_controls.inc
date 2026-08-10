drogon::HttpResponsePtr requireCommissionerDraftControl(const LeagueAccess &access,
                                                        const std::string &action) {
    if (auto response = requireAccessResponse(access)) return response;
    if (!access.commissioner) {
        return errorResponse(drogon::k403Forbidden,
                             "Only the league commissioner can " + action + " the draft.",
                             "commissioner_required");
    }
    return nullptr;
}

drogon::HttpResponsePtr pauseDraft(const drogon::HttpRequestPtr &request,
                                   const std::string &leagueId,
                                   const std::string &email) {
    auto connection = connectDb();
    if (!connection || !begin(connection.get()) || !lockDraft(connection.get(), leagueId)) return unavailable();
    const auto access = leagueAccess(connection.get(), leagueId, email);
    if (auto response = requireCommissionerDraftControl(access, "pause")) {
        rollback(connection.get());
        return response;
    }
    const auto key = operationKey(request);
    if (const auto replay = operationReplay(connection.get(), leagueId, key)) {
        if (*replay != "pause") {
            rollback(connection.get());
            return errorResponse(drogon::k409Conflict, "This operation key was already used.", "draft_operation_key_conflict");
        }
        if (!commit(connection.get())) return unavailable();
        auto payload = draftPayload(connection.get(), leagueId, email);
        payload["idempotentReplay"] = true;
        return jsonResponse(payload);
    }

    (void)resolveDueAutoDrafts(connection.get(), leagueId);
    auto state = execute(connection.get(),
        "SELECT status, version, current_pick, to_json(draft_order)::text, pick_clock_seconds, "
        "COALESCE(GREATEST(1, CEIL(EXTRACT(EPOCH FROM (pick_deadline - NOW()))))::integer, pick_clock_seconds) "
        "FROM draft_states WHERE league_id = $1 FOR UPDATE",
        {leagueId});
    if (!tuplesOk(state) || PQntuples(state.get()) == 0) {
        rollback(connection.get());
        return unavailable();
    }
    const auto status = cell(state.get(), 0, 0);
    const auto version = cellInt64(state.get(), 0, 1, 0);
    const int currentPick = cellInt(state.get(), 0, 2, 1);
    const auto order = jsonFromString(cell(state.get(), 0, 3), Json::Value{Json::arrayValue});
    const auto currentManager = cff::draft_lifecycle::managerForPick(order, currentPick, access.draftType);
    if (auto response = requireExpectedVersion(request, version, currentPick, currentManager)) {
        rollback(connection.get());
        return response;
    }
    if (status != "open") {
        rollback(connection.get());
        return errorResponse(drogon::k409Conflict,
                             status == "paused" ? "The draft is already paused." : "Only an in-progress draft can be paused.",
                             "draft_pause_not_allowed");
    }
    const int remaining = cellInt(state.get(), 0, 5, cellInt(state.get(), 0, 4, 90));
    auto updated = execute(connection.get(),
        "UPDATE draft_states SET status = 'paused', paused_remaining_seconds = $2::integer, "
        "pick_deadline = NULL, version = version + 1, updated_at = NOW() "
        "WHERE league_id = $1 RETURNING version",
        {leagueId, std::to_string(std::max(1, remaining))});
    if (!tuplesOk(updated) || PQntuples(updated.get()) == 0) {
        rollback(connection.get());
        return unavailable();
    }
    const auto newVersion = cellInt64(updated.get(), 0, 0, 0);
    (void)logDraftActivity(connection.get(), leagueId, email, "draft_paused", "Draft paused by commissioner.", currentPick);
    if (!recordOperation(connection.get(), leagueId, key, "pause", newVersion) || !commit(connection.get())) {
        rollback(connection.get());
        return unavailable();
    }
    return jsonResponse(draftPayload(connection.get(), leagueId, email));
}

drogon::HttpResponsePtr resumeDraft(const drogon::HttpRequestPtr &request,
                                    const std::string &leagueId,
                                    const std::string &email) {
    auto connection = connectDb();
    if (!connection || !begin(connection.get()) || !lockDraft(connection.get(), leagueId)) return unavailable();
    const auto access = leagueAccess(connection.get(), leagueId, email);
    if (auto response = requireCommissionerDraftControl(access, "resume")) {
        rollback(connection.get());
        return response;
    }
    const auto key = operationKey(request);
    if (const auto replay = operationReplay(connection.get(), leagueId, key)) {
        if (*replay != "resume") {
            rollback(connection.get());
            return errorResponse(drogon::k409Conflict, "This operation key was already used.", "draft_operation_key_conflict");
        }
        if (!commit(connection.get())) return unavailable();
        auto payload = draftPayload(connection.get(), leagueId, email);
        payload["idempotentReplay"] = true;
        return jsonResponse(payload);
    }
    auto state = execute(connection.get(),
        "SELECT status, version, current_pick, to_json(draft_order)::text, "
        "COALESCE(paused_remaining_seconds, pick_clock_seconds), pick_clock_seconds "
        "FROM draft_states WHERE league_id = $1 FOR UPDATE",
        {leagueId});
    if (!tuplesOk(state) || PQntuples(state.get()) == 0) {
        rollback(connection.get());
        return unavailable();
    }
    const auto status = cell(state.get(), 0, 0);
    const auto version = cellInt64(state.get(), 0, 1, 0);
    const int currentPick = cellInt(state.get(), 0, 2, 1);
    const auto order = jsonFromString(cell(state.get(), 0, 3), Json::Value{Json::arrayValue});
    const auto currentManager = cff::draft_lifecycle::managerForPick(order, currentPick, access.draftType);
    if (auto response = requireExpectedVersion(request, version, currentPick, currentManager)) {
        rollback(connection.get());
        return response;
    }
    if (status != "paused") {
        rollback(connection.get());
        return errorResponse(drogon::k409Conflict, "Only a paused draft can be resumed.", "draft_resume_not_allowed");
    }
    const int remaining = std::max(1, cellInt(state.get(), 0, 4, cellInt(state.get(), 0, 5, 90)));
    auto updated = execute(connection.get(),
        "UPDATE draft_states SET status = 'open', "
        "pick_deadline = NOW() + ($2::integer * INTERVAL '1 second'), paused_remaining_seconds = NULL, "
        "version = version + 1, updated_at = NOW() WHERE league_id = $1 RETURNING version",
        {leagueId, std::to_string(remaining)});
    if (!tuplesOk(updated) || PQntuples(updated.get()) == 0) {
        rollback(connection.get());
        return unavailable();
    }
    const auto newVersion = cellInt64(updated.get(), 0, 0, 0);
    (void)logDraftActivity(connection.get(), leagueId, email, "draft_resumed", "Draft resumed by commissioner.", currentPick);
    if (!recordOperation(connection.get(), leagueId, key, "resume", newVersion) || !commit(connection.get())) {
        rollback(connection.get());
        return unavailable();
    }
    return jsonResponse(draftPayload(connection.get(), leagueId, email));
}

drogon::HttpResponsePtr cancelDraft(const drogon::HttpRequestPtr &request,
                                    const std::string &leagueId,
                                    const std::string &email) {
    auto connection = connectDb();
    if (!connection || !begin(connection.get()) || !lockDraft(connection.get(), leagueId)) return unavailable();
    const auto access = leagueAccess(connection.get(), leagueId, email);
    if (auto response = requireCommissionerDraftControl(access, "cancel")) {
        rollback(connection.get());
        return response;
    }
    const auto key = operationKey(request);
    if (const auto replay = operationReplay(connection.get(), leagueId, key)) {
        if (*replay != "cancel") {
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
        "SELECT status, version, current_pick, to_json(draft_order)::text, "
        "(SELECT COUNT(*) FROM draft_picks WHERE league_id = $1) "
        "FROM draft_states WHERE league_id = $1 FOR UPDATE",
        {leagueId});
    if (!tuplesOk(state) || PQntuples(state.get()) == 0) {
        rollback(connection.get());
        return unavailable();
    }
    const auto version = cellInt64(state.get(), 0, 1, 0);
    const int currentPick = cellInt(state.get(), 0, 2, 1);
    const auto order = jsonFromString(cell(state.get(), 0, 3), Json::Value{Json::arrayValue});
    const auto currentManager = cff::draft_lifecycle::managerForPick(order, currentPick, access.draftType);
    if (auto response = requireExpectedVersion(request, version, currentPick, currentManager)) {
        rollback(connection.get());
        return response;
    }
    if (cellInt(state.get(), 0, 4, 0) > 0 || cell(state.get(), 0, 0) == "open" || cell(state.get(), 0, 0) == "paused") {
        rollback(connection.get());
        return errorResponse(drogon::k409Conflict,
                             "A draft with confirmed picks must be reset rather than cancelled.",
                             "draft_cancel_not_allowed");
    }
    auto updated = execute(connection.get(),
        "UPDATE draft_states SET status = 'cancelled', current_pick = 1, pick_deadline = NULL, "
        "paused_remaining_seconds = NULL, started_at = NULL, completed_at = NULL, "
        "version = version + 1, updated_at = NOW() WHERE league_id = $1 RETURNING version",
        {leagueId});
    if (!tuplesOk(updated) || PQntuples(updated.get()) == 0) {
        rollback(connection.get());
        return unavailable();
    }
    const auto newVersion = cellInt64(updated.get(), 0, 0, 0);
    (void)logDraftActivity(connection.get(), leagueId, email, "draft_cancelled", "Draft cancelled before start.");
    if (!recordOperation(connection.get(), leagueId, key, "cancel", newVersion) || !commit(connection.get())) {
        rollback(connection.get());
        return unavailable();
    }
    return jsonResponse(draftPayload(connection.get(), leagueId, email));
}

drogon::HttpResponsePtr saveDraftSettings(const drogon::HttpRequestPtr &request,
                                          const std::string &leagueId,
                                          const std::string &email) {
    auto connection = connectDb();
    if (!connection || !begin(connection.get()) || !lockDraft(connection.get(), leagueId)) return unavailable();
    const auto access = leagueAccess(connection.get(), leagueId, email);
    if (auto response = requireCommissionerDraftControl(access, "configure")) {
        rollback(connection.get());
        return response;
    }
    const auto key = operationKey(request);
    if (const auto replay = operationReplay(connection.get(), leagueId, key)) {
        if (*replay != "settings") {
            rollback(connection.get());
            return errorResponse(drogon::k409Conflict, "This operation key was already used.", "draft_operation_key_conflict");
        }
        if (!commit(connection.get())) return unavailable();
        auto payload = draftPayload(connection.get(), leagueId, email);
        payload["idempotentReplay"] = true;
        return jsonResponse(payload);
    }
    const auto body = request->getJsonObject();
    if (!body || !body->isObject()) {
        rollback(connection.get());
        return errorResponse(drogon::k400BadRequest, "Draft settings are required.", "draft_settings_required");
    }
    const int pickClockSeconds = body->get("pickClockSeconds", 90).asInt();
    const auto timezone = trim(body->get("timezone", "UTC").asString());
    if (pickClockSeconds < 15 || pickClockSeconds > 600) {
        rollback(connection.get());
        return errorResponse(drogon::k400BadRequest,
                             "Pick duration must be between 15 and 600 seconds.",
                             "draft_pick_clock_invalid");
    }
    if (timezone.empty() || timezone.size() > 64) {
        rollback(connection.get());
        return errorResponse(drogon::k400BadRequest, "A valid draft timezone is required.", "draft_timezone_invalid");
    }
    const auto managers = activeManagers(connection.get(), leagueId);
    (void)ensureDraftState(connection.get(), leagueId, managers);
    auto state = execute(connection.get(),
        "SELECT status, version, current_pick, to_json(draft_order)::text, "
        "(SELECT COUNT(*) FROM draft_picks WHERE league_id = $1) "
        "FROM draft_states WHERE league_id = $1 FOR UPDATE",
        {leagueId});
    if (!tuplesOk(state) || PQntuples(state.get()) == 0) {
        rollback(connection.get());
        return unavailable();
    }
    const auto version = cellInt64(state.get(), 0, 1, 0);
    const int currentPick = cellInt(state.get(), 0, 2, 1);
    const auto order = jsonFromString(cell(state.get(), 0, 3), Json::Value{Json::arrayValue});
    const auto currentManager = cff::draft_lifecycle::managerForPick(order, currentPick, access.draftType);
    if (auto response = requireExpectedVersion(request, version, currentPick, currentManager)) {
        rollback(connection.get());
        return response;
    }
    const auto status = cell(state.get(), 0, 0);
    if ((status != "not_started" && status != "cancelled") || cellInt(state.get(), 0, 4, 0) > 0) {
        rollback(connection.get());
        return errorResponse(drogon::k409Conflict,
                             "Draft settings are locked after the draft starts.",
                             "draft_settings_locked");
    }
    auto stateUpdate = execute(connection.get(),
        "UPDATE draft_states SET pick_clock_seconds = $2::integer, version = version + 1, updated_at = NOW() "
        "WHERE league_id = $1 RETURNING version",
        {leagueId, std::to_string(pickClockSeconds)});
    auto leagueUpdate = execute(connection.get(),
        "UPDATE leagues SET draft_timezone = $2, updated_at = NOW() WHERE id = $1",
        {leagueId, timezone});
    if (!tuplesOk(stateUpdate) || PQntuples(stateUpdate.get()) == 0 || !commandOk(leagueUpdate)) {
        rollback(connection.get());
        return unavailable();
    }
    const auto newVersion = cellInt64(stateUpdate.get(), 0, 0, 0);
    Json::Value metadata(Json::objectValue);
    metadata["pickClockSeconds"] = pickClockSeconds;
    metadata["timezone"] = timezone;
    (void)logDraftActivity(connection.get(), leagueId, email, "draft_settings_updated", "Draft settings updated.", 0, "", metadata);
    if (!recordOperation(connection.get(), leagueId, key, "settings", newVersion) || !commit(connection.get())) {
        rollback(connection.get());
        return unavailable();
    }
    return jsonResponse(draftPayload(connection.get(), leagueId, email));
}
