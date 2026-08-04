Json::Value waiverStatePayload(PGconn *connection,
                               const std::string &leagueId,
                               const std::string &email,
                               const LeagueAccess &access,
                               bool commissioner,
                               long long versionOverride = -1) {
    const auto version = versionOverride >= 0
        ? versionOverride
        : waiverVersion(connection, leagueId);
    Json::Value payload(Json::objectValue);
    payload["leagueId"] = leagueId;
    payload["version"] = Json::Int64(version);
    payload["claims"] = waiverClaimsPayload(connection, leagueId, email, commissioner);
    payload["priority"] = waiverPriorityPayload(connection, leagueId);
    payload["roster"] = rosterPayload(connection, leagueId, email);
    payload["rosterVersion"] = Json::Int64(rosterVersion(connection, leagueId, email));
    payload["unavailablePlayerIds"] = unavailablePlayerIds(connection, leagueId);
    payload["waiverRules"] = access.waiverRules;
    payload["waiverModeActive"] = cff::league_waiver::modeActive(access.waiverRules);
    payload["deadlinePassed"] = cff::league_waiver::deadlinePassed(access.waiverRules);
    payload["lineupLocked"] = lineupLocked(connection, leagueId);
    payload["commissioner"] = commissioner;
    int pending = 0;
    for (const auto &claim : payload["claims"]) {
        if (claim.get("status", "").asString() == "Pending") ++pending;
    }
    payload["pendingCount"] = pending;
    payload["canProcess"] = commissioner
        && payload["deadlinePassed"].asBool()
        && !payload["lineupLocked"].asBool();

    auto state = execute(connection,
        "SELECT COALESCE(to_char(last_processed_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), ''), "
        "COALESCE(last_processing_run_id, '') FROM waiver_states WHERE league_id = $1",
        {leagueId});
    if (tuplesOk(state) && PQntuples(state.get()) > 0) {
        payload["lastProcessedAt"] = cell(state.get(), 0, 0);
        payload["lastProcessingRunId"] = cell(state.get(), 0, 1);
    } else {
        payload["lastProcessedAt"] = "";
        payload["lastProcessingRunId"] = "";
    }
    return payload;
}

Json::Value currentWaiverStateDetails(PGconn *connection,
                                      const std::string &leagueId,
                                      const std::string &email,
                                      const LeagueAccess &access,
                                      bool commissioner) {
    Json::Value details(Json::objectValue);
    details["state"] = waiverStatePayload(connection, leagueId, email, access, commissioner);
    return details;
}

Json::Value claimFromState(const Json::Value &state, const std::string &claimId) {
    if (!state.isObject() || !state["claims"].isArray()) return Json::Value{Json::objectValue};
    for (const auto &claim : state["claims"]) {
        if (claim.get("id", "").asString() == claimId) return claim;
    }
    return Json::Value{Json::objectValue};
}
