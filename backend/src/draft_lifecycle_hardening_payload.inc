Json::Value draftPayload(PGconn *connection,
                         const std::string &leagueId,
                         const std::string &email,
                         bool updatePresence = true) {
    const auto access = leagueAccess(connection, leagueId, email);
    const auto managers = activeManagers(connection, leagueId);
    (void)ensureDraftState(connection, leagueId, managers);
    if (updatePresence && access.member) (void)touchPresence(connection, leagueId, email);

    Json::Value payload(Json::objectValue);
    auto state = execute(connection,
        "SELECT ds.status, ds.current_pick, to_json(ds.draft_order)::text, "
        "ds.pick_clock_seconds, "
        "COALESCE(to_char(ds.pick_deadline AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), ''), "
        "COALESCE(to_char(ds.started_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), ''), "
        "ds.version, "
        "COALESCE(to_char(ds.completed_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), ''), "
        "l.draft_type, "
        "(l.draft_lobby_open OR (l.draft_date IS NOT NULL AND l.draft_date <= NOW() + INTERVAL '30 minutes')), "
        "l.roster_rules::text "
        "FROM draft_states ds JOIN leagues l ON l.id = ds.league_id WHERE ds.league_id = $1",
        {leagueId});
    if (!tuplesOk(state) || PQntuples(state.get()) == 0) return payload;

    const auto order = jsonFromString(cell(state.get(), 0, 2), Json::Value{Json::arrayValue});
    const int currentPick = cellInt(state.get(), 0, 1, 1);
    const auto status = cell(state.get(), 0, 0);
    const auto draftType = cell(state.get(), 0, 8).empty() ? "snake" : cell(state.get(), 0, 8);
    const auto rules = jsonFromString(cell(state.get(), 0, 10));
    const long long version = cellInt64(state.get(), 0, 6, 0);

    payload["status"] = status;
    payload["currentPick"] = currentPick;
    payload["draftOrder"] = order;
    payload["pickClockSeconds"] = cellInt(state.get(), 0, 3, 90);
    payload["pickDeadline"] = cell(state.get(), 0, 4);
    payload["startedAt"] = cell(state.get(), 0, 5);
    payload["version"] = Json::Int64(version);
    payload["revision"] = Json::Int64(version);
    payload["completedAt"] = cell(state.get(), 0, 7);
    payload["draftType"] = draftType;
    payload["lobbyOpen"] = cell(state.get(), 0, 9) == "t";
    payload["activeManagerCount"] = static_cast<int>(managers.size());
    payload["rosterSlotsPerManager"] = cff::draft_lifecycle::rosterSlotsPerManager(rules);
    payload["totalPicks"] = cff::draft_lifecycle::totalDraftPicks(static_cast<int>(managers.size()), rules);
    payload["currentManager"] = status == "open"
        ? cff::draft_lifecycle::managerForPick(order, currentPick, draftType)
        : "";

    Json::Value picks(Json::arrayValue);
    auto pickResult = execute(connection,
        "SELECT pick_number, lower(manager_email), player_id, player_snapshot::text, "
        "COALESCE(to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), ''), "
        "COALESCE(selection_source, 'manual') "
        "FROM draft_picks WHERE league_id = $1 ORDER BY pick_number",
        {leagueId});
    if (tuplesOk(pickResult)) {
        for (int row = 0; row < PQntuples(pickResult.get()); ++row) {
            Json::Value pick(Json::objectValue);
            pick["pickNumber"] = cellInt(pickResult.get(), row, 0, row + 1);
            pick["managerEmail"] = cell(pickResult.get(), row, 1);
            pick["playerId"] = cell(pickResult.get(), row, 2);
            pick["player"] = jsonFromString(cell(pickResult.get(), row, 3));
            pick["createdAt"] = cell(pickResult.get(), row, 4);
            pick["selectionSource"] = cell(pickResult.get(), row, 5);
            pick["automatic"] = cell(pickResult.get(), row, 5) != "manual";
            picks.append(pick);
        }
    }
    payload["picks"] = picks;
    payload["picksRemaining"] = std::max(0, payload["totalPicks"].asInt() - static_cast<int>(picks.size()));

    payload["queue"] = Json::Value{Json::arrayValue};
    auto queueResult = execute(connection,
        "SELECT queue::text FROM draft_queues WHERE league_id = $1 AND lower(manager_email) = lower($2)",
        {leagueId, email});
    if (tuplesOk(queueResult) && PQntuples(queueResult.get()) > 0) {
        payload["queue"] = jsonFromString(cell(queueResult.get(), 0, 0), Json::Value{Json::arrayValue});
    }

    const auto readiness = readinessPayload(connection, leagueId);
    payload["readiness"] = readiness;
    int readyCount = 0;
    int connectedCount = 0;
    for (const auto &entry : readiness) {
        if (entry.get("ready", false).asBool()) ++readyCount;
        if (entry.get("connected", false).asBool()) ++connectedCount;
    }
    payload["readyCount"] = readyCount;
    payload["connectedCount"] = connectedCount;
    payload["allReady"] = cff::draft_lifecycle::allManagersReady(managers, readiness);
    Json::Value activity(Json::arrayValue);
    auto activityResult = execute(connection,
        "SELECT event_type, COALESCE(manager_email, ''), message, COALESCE(pick_number, 0), "
        "COALESCE(player_id, ''), metadata::text, "
        "COALESCE(to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), '') "
        "FROM draft_activity_log WHERE league_id = $1 ORDER BY created_at DESC, id DESC LIMIT 100",
        {leagueId});
    if (tuplesOk(activityResult)) {
        for (int row = 0; row < PQntuples(activityResult.get()); ++row) {
            Json::Value entry(Json::objectValue);
            entry["eventType"] = cell(activityResult.get(), row, 0);
            entry["managerEmail"] = cell(activityResult.get(), row, 1);
            entry["message"] = cell(activityResult.get(), row, 2);
            entry["pickNumber"] = cellInt(activityResult.get(), row, 3, 0);
            entry["playerId"] = cell(activityResult.get(), row, 4);
            entry["metadata"] = jsonFromString(cell(activityResult.get(), row, 5));
            entry["createdAt"] = cell(activityResult.get(), row, 6);
            activity.append(entry);
        }
    }
    payload["activity"] = activity;
    auto serverTime = execute(connection,
        "SELECT to_char(NOW() AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"')",
        {});
    payload["serverTime"] = tuplesOk(serverTime) && PQntuples(serverTime.get()) > 0
        ? cell(serverTime.get(), 0, 0)
        : "";
    return payload;
}

drogon::HttpResponsePtr unavailable() {
    return errorResponse(drogon::k503ServiceUnavailable,
                         "Draft storage is temporarily unavailable.",
                         "draft_storage_unavailable",
                         true);
}

drogon::HttpResponsePtr requireAccessResponse(const LeagueAccess &access) {
    if (!access.exists) {
        return errorResponse(drogon::k404NotFound, "League not found.", "league_not_found");
    }
    if (!access.member) {
        return errorResponse(drogon::k403Forbidden, "Active league membership is required.", "draft_membership_required");
    }
    return nullptr;
}

std::optional<long long> expectedVersion(const drogon::HttpRequestPtr &request) {
    const auto body = request->getJsonObject();
    if (!body || !body->isObject() || !body->isMember("expectedVersion")
        || !(*body)["expectedVersion"].isNumeric()) {
        return std::nullopt;
    }
    return (*body)["expectedVersion"].asInt64();
}

drogon::HttpResponsePtr draftVersionConflict(long long currentVersion,
                                              int currentPick = 1,
                                              const std::string &currentManager = "") {
    Json::Value details(Json::objectValue);
    details["version"] = Json::Int64(currentVersion);
    details["revision"] = Json::Int64(currentVersion);
    details["currentPick"] = currentPick;
    if (!currentManager.empty()) details["currentManager"] = currentManager;
    return errorResponse(drogon::k409Conflict,
                         "Draft state changed before this action was submitted. Refresh and try again.",
                         "draft_state_conflict",
                         true,
                         details);
}

drogon::HttpResponsePtr requireExpectedVersion(const drogon::HttpRequestPtr &request,
                                                long long currentVersion,
                                                int currentPick = 1,
                                                const std::string &currentManager = "") {
    const auto expected = expectedVersion(request);
    if (!expected) {
        return errorResponse(drogon::k409Conflict,
                             "A confirmed draft version is required for this action.",
                             "draft_precondition_required",
                             true);
    }
    if (*expected != currentVersion) {
        return draftVersionConflict(currentVersion, currentPick, currentManager);
    }
    return nullptr;
}

drogon::HttpResponsePtr getDraft(const std::string &leagueId,
                                 const std::string &email) {
    auto connection = connectDb();
    if (!connection) return unavailable();
    if (!begin(connection.get()) || !lockDraft(connection.get(), leagueId)) return unavailable();
    const auto access = leagueAccess(connection.get(), leagueId, email);
    if (auto response = requireAccessResponse(access)) {
        rollback(connection.get());
        return response;
    }
    (void)resolveDueAutoDrafts(connection.get(), leagueId);
    auto payload = draftPayload(connection.get(), leagueId, email);
    if (!commit(connection.get())) return unavailable();
    return jsonResponse(payload);
}

drogon::HttpResponsePtr setReadiness(const drogon::HttpRequestPtr &request,
                                     const std::string &leagueId,
                                     const std::string &email) {
    auto connection = connectDb();
    if (!connection) return unavailable();
    const auto access = leagueAccess(connection.get(), leagueId, email);
    if (auto response = requireAccessResponse(access)) return response;
    const auto body = request->getJsonObject();
    const bool ready = body && body->isObject() && body->get("ready", false).asBool();
    auto result = execute(connection.get(),
        "INSERT INTO draft_readiness (league_id, manager_email, ready, last_seen_at, updated_at) "
        "VALUES ($1, $2, $3::boolean, NOW(), NOW()) "
        "ON CONFLICT (league_id, manager_email) DO UPDATE "
        "SET ready = EXCLUDED.ready, last_seen_at = NOW(), updated_at = NOW()",
        {leagueId, email, ready ? "true" : "false"});
    if (!commandOk(result)) return unavailable();
    auto payload = draftPayload(connection.get(), leagueId, email, false);
    payload["message"] = ready ? "Manager is ready." : "Manager is not ready.";
    return jsonResponse(payload);
}

drogon::HttpResponsePtr setAutoDraft(const drogon::HttpRequestPtr &request,
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
    const bool enabled = body && body->isObject() && body->get("enabled", false).asBool();
    auto result = execute(connection.get(),
        "INSERT INTO draft_readiness "
        "(league_id, manager_email, ready, auto_draft_enabled, consecutive_missed_picks, last_seen_at, updated_at) "
        "VALUES ($1, $2, FALSE, $3::boolean, 0, NOW(), NOW()) "
        "ON CONFLICT (league_id, manager_email) DO UPDATE "
        "SET auto_draft_enabled = EXCLUDED.auto_draft_enabled, last_seen_at = NOW(), updated_at = NOW()",
        {leagueId, email, enabled ? "true" : "false"});
    if (!commandOk(result)) {
        rollback(connection.get());
        return unavailable();
    }
    (void)logDraftActivity(
        connection.get(),
        leagueId,
        email,
        enabled ? "auto_draft_enabled" : "auto_draft_disabled",
        enabled ? "Auto-draft enabled." : "Auto-draft disabled.");
    (void)resolveDueAutoDrafts(connection.get(), leagueId);
    auto payload = draftPayload(connection.get(), leagueId, email, false);
    payload["message"] = enabled ? "Auto-draft enabled." : "Auto-draft disabled.";
    if (!commit(connection.get())) return unavailable();
    return jsonResponse(payload);
}
