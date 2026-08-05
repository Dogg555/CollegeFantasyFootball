drogon::HttpResponsePtr makePick(const drogon::HttpRequestPtr &request,
                                 const std::string &leagueId,
                                 const std::string &email) {
    auto connection = connectDb();
    if (!connection || !begin(connection.get()) || !lockDraft(connection.get(), leagueId)) return unavailable();
    const auto access = leagueAccess(connection.get(), leagueId, email);
    if (auto response = requireAccessResponse(access)) {
        rollback(connection.get());
        return response;
    }
    const auto body = request->getJsonObject();
    if (!body || !body->isObject() || !body->isMember("player") || !(*body)["player"].isObject()) {
        rollback(connection.get());
        return errorResponse(drogon::k400BadRequest, "Player data is required.", "draft_player_required");
    }
    const auto player = (*body)["player"];
    const auto playerId = trim(player.get("id", "").asString());
    if (playerId.empty()) {
        rollback(connection.get());
        return errorResponse(drogon::k400BadRequest, "Player ID is required.", "draft_player_id_required");
    }
    const auto key = operationKey(request);
    if (const auto replay = operationReplay(connection.get(), leagueId, key)) {
        if (*replay != "pick") {
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
        "SELECT ds.status, ds.current_pick, to_json(ds.draft_order)::text, ds.pick_clock_seconds, "
        "ds.version, l.draft_type, l.roster_rules::text "
        "FROM draft_states ds JOIN leagues l ON l.id = ds.league_id "
        "WHERE ds.league_id = $1 FOR UPDATE OF ds",
        {leagueId});
    if (!tuplesOk(state) || PQntuples(state.get()) == 0) {
        rollback(connection.get());
        return unavailable();
    }
    const auto status = cell(state.get(), 0, 0);
    const int currentPick = cellInt(state.get(), 0, 1, 1);
    const auto order = jsonFromString(cell(state.get(), 0, 2), Json::Value{Json::arrayValue});
    const int clockSeconds = cellInt(state.get(), 0, 3, 90);
    const long long version = cellInt64(state.get(), 0, 4, 0);
    const auto draftType = cell(state.get(), 0, 5).empty() ? "snake" : cell(state.get(), 0, 5);
    const auto rules = jsonFromString(cell(state.get(), 0, 6));

    if (status != "open") {
        rollback(connection.get());
        return errorResponse(drogon::k409Conflict, "The draft is not currently open.", "draft_not_open");
    }
    const auto currentManager = cff::draft_lifecycle::managerForPick(order, currentPick, draftType);
    if (!body->isMember("expectedPick") || !(*body)["expectedPick"].isInt()) {
        rollback(connection.get());
        return errorResponse(drogon::k409Conflict,
                             "A confirmed pick number is required for this action.",
                             "draft_precondition_required",
                             true);
    }
    if (auto response = requireExpectedVersion(request, version, currentPick, currentManager)) {
        rollback(connection.get());
        return response;
    }
    const int expectedPick = (*body)["expectedPick"].asInt();
    if (expectedPick != currentPick) {
        rollback(connection.get());
        return draftVersionConflict(version, currentPick, currentManager);
    }
    if (currentManager != email) {
        rollback(connection.get());
        Json::Value details(Json::objectValue);
        details["currentManager"] = currentManager;
        details["currentPick"] = currentPick;
        details["version"] = Json::Int64(version);
        return errorResponse(drogon::k409Conflict,
                             "Another manager is currently on the clock.",
                             "draft_not_your_turn",
                             false,
                             details);
    }
    auto duplicate = execute(connection.get(),
        "SELECT pick_number FROM draft_picks WHERE league_id = $1 AND player_id = $2 LIMIT 1",
        {leagueId, playerId});
    if (!tuplesOk(duplicate)) {
        rollback(connection.get());
        return unavailable();
    }
    if (PQntuples(duplicate.get()) > 0) {
        rollback(connection.get());
        Json::Value details(Json::objectValue);
        details["pickNumber"] = cellInt(duplicate.get(), 0, 0, 0);
        return errorResponse(drogon::k409Conflict,
                             "That player has already been drafted.",
                             "player_already_drafted",
                             false,
                             details);
    }
    const auto counts = rosterCounts(connection.get(), leagueId, email);
    const auto slot = cff::league_roster::preferredRosterSlot(player, rules, counts);
    if (!slot) {
        rollback(connection.get());
        return errorResponse(drogon::k409Conflict,
                             "This manager's roster is full.",
                             "draft_roster_full");
    }
    AutoDraftCandidate candidate;
    candidate.player = player;
    candidate.playerId = playerId;
    candidate.rosterSlot = *slot;
    candidate.source = "manual";
    const auto newVersion = persistDraftSelection(
        connection.get(),
        leagueId,
        email,
        currentPick,
        clockSeconds,
        rules,
        static_cast<int>(managers.size()),
        candidate,
        "manual",
        key,
        false);
    if (!newVersion) {
        rollback(connection.get());
        return errorResponse(drogon::k409Conflict,
                             "This draft pick was already claimed by another request.",
                             "draft_pick_conflict",
                             true);
    }
    if (!recordOperation(connection.get(), leagueId, key, "pick", *newVersion) || !commit(connection.get())) {
        rollback(connection.get());
        return unavailable();
    }
    auto payload = draftPayload(connection.get(), leagueId, email);
    payload["acceptedPick"] = currentPick;
    payload["acceptedPlayerId"] = playerId;
    return jsonResponse(payload, drogon::k201Created);
}
