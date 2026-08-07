Json::Value waiverStatePayload(PGconn *connection,
                               const std::string &leagueId,
                               const std::string &email,
                               const LeagueAccess &access,
                               bool commissioner,
                               long long versionOverride = -1) {
    const auto version = versionOverride >= 0
        ? versionOverride
        : waiverVersion(connection, leagueId);
    const auto processingPeriod = effectiveWaiverProcessingPeriod(connection, leagueId, access.waiverRules);
    const auto lastProcessingPeriod = lastCompletedWaiverPeriod(connection, leagueId);
    Json::Value payload(Json::objectValue);
    payload["leagueId"] = leagueId;
    payload["version"] = Json::Int64(version);
    payload["claims"] = phase5WaiverClaimsPayload(connection, leagueId, email, commissioner);
    payload["priority"] = waiverPriorityPayload(connection, leagueId);
    payload["roster"] = rosterPayload(connection, leagueId, email);
    payload["rosterVersion"] = Json::Int64(rosterVersion(connection, leagueId, email));
    payload["unavailablePlayerIds"] = unavailablePlayerIds(connection, leagueId);
    payload["waiverRules"] = access.waiverRules;
    payload["waiverModeActive"] = cff::league_waiver::modeActive(access.waiverRules);
    payload["deadlinePassed"] = cff::league_waiver::deadlinePassed(access.waiverRules);
    payload["claimsMutable"] = !payload["deadlinePassed"].asBool();
    payload["lineupLocked"] = false;
    payload["commissioner"] = commissioner;
    payload["processingPeriod"] = processingPeriod;
    payload["lastProcessingPeriod"] = lastProcessingPeriod;
    payload["periodProcessed"] = !processingPeriod.empty() && processingPeriod == lastProcessingPeriod;
    int pending = 0;
    for (const auto &claim : payload["claims"]) {
        if (claim.get("status", "").asString() == "Pending") ++pending;
    }
    payload["pendingCount"] = pending;
    payload["canProcess"] = commissioner
        && payload["deadlinePassed"].asBool()
        && (!payload["periodProcessed"].asBool() || processingPeriod.empty());

    auto state = execute(connection,
        "SELECT COALESCE(to_char(last_processed_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), ''), "
        "COALESCE(last_processing_run_id, ''), COALESCE(last_processing_period, '') "
        "FROM waiver_states WHERE league_id = $1",
        {leagueId});
    if (tuplesOk(state) && PQntuples(state.get()) > 0) {
        payload["lastProcessedAt"] = cell(state.get(), 0, 0);
        payload["lastProcessingRunId"] = cell(state.get(), 0, 1);
        payload["lastProcessingPeriod"] = cell(state.get(), 0, 2);
    } else {
        payload["lastProcessedAt"] = "";
        payload["lastProcessingRunId"] = "";
        payload["lastProcessingPeriod"] = "";
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
