Json::Value scheduleStatePayload(PGconn *connection,
                                 const std::string &leagueId,
                                 const std::string &email,
                                 const ScheduleAccess &access,
                                 const ScheduleStateRecord &state) {
    Json::Value payload(Json::objectValue);
    payload["leagueId"] = leagueId;
    payload["season"] = state.season;
    payload["weeks"] = state.weeks;
    payload["currentWeek"] = state.currentWeek;
    payload["version"] = Json::Int64(state.version);
    payload["scheduleVersion"] = Json::Int64(state.version);
    payload["scheduleHash"] = state.scheduleHash;
    payload["generatedByEmail"] = state.generatedBy;
    payload["generatedAt"] = state.generatedAt;
    payload["commissioner"] = access.commissioner;
    payload["viewerEmail"] = email;
    payload["matchups"] = scheduleMatchupsPayload(connection, leagueId, state.season);
    payload["weekControls"] = weekControlsPayload(connection, leagueId, state.season);
    payload["serverTime"] = drogon::utils::getHttpFullDate(std::chrono::system_clock::now());
    return payload;
}

Json::Value scheduleConflictDetails(PGconn *connection,
                                    const std::string &leagueId,
                                    const std::string &email,
                                    const ScheduleAccess &access,
                                    const ScheduleStateRecord &state) {
    Json::Value details(Json::objectValue);
    details["currentState"] = scheduleStatePayload(connection, leagueId, email, access, state);
    details["currentVersion"] = Json::Int64(state.version);
    return details;
}

drogon::HttpResponsePtr scheduleStorageUnavailable() {
    return errorResponse(drogon::k503ServiceUnavailable,
                         "Schedule storage is temporarily unavailable.",
                         "schedule_storage_unavailable",
                         true);
}
