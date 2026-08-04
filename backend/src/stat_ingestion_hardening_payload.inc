Json::Value statStatePayload(PGconn *connection,
                             const StatStateRecord &state,
                             const std::string &actor) {
    Json::Value payload(Json::objectValue);
    payload["season"] = state.season;
    payload["week"] = state.week;
    payload["version"] = Json::Int64(state.version);
    payload["sourceRevision"] = Json::Int64(state.sourceRevision);
    payload["status"] = state.status;
    payload["activeRunId"] = Json::Int64(state.activeRunId);
    payload["lastSuccessAt"] = state.lastSuccessAt;
    payload["ageSeconds"] = Json::Int64(state.ageSeconds);
    payload["staleAfterSeconds"] = state.staleAfterSeconds;
    payload["fresh"] = cff::stat_ingestion_lifecycle::sourceFresh(
        state.ageSeconds, state.staleAfterSeconds);
    payload["nextRetryAt"] = state.nextRetryAt;
    payload["providerStatus"] = state.providerStatus;
    payload["lastError"] = state.lastError;
    payload["inserted"] = state.inserted;
    payload["corrected"] = state.corrected;
    payload["unchanged"] = state.unchanged;
    payload["actorId"] = actor;
    payload["recalculationQueue"] = queuePayload(connection, state.season, state.week);
    if (state.activeRunId > 0) {
        const auto run = runRecord(connection, state.activeRunId);
        if (run.id > 0) {
            payload["activeRun"]["id"] = Json::Int64(run.id);
            payload["activeRun"]["runKey"] = run.runKey;
            payload["activeRun"]["ownerId"] = run.ownerId;
            payload["activeRun"]["status"] = run.status;
            payload["activeRun"]["attempt"] = run.attempt;
            payload["activeRun"]["heartbeatAt"] = run.heartbeatAt;
            payload["activeRun"]["leaseExpiresAt"] = run.leaseExpiresAt;
            payload["activeRun"]["leaseActive"] = run.leaseActive;
        }
    }
    return payload;
}

Json::Value statConflictDetails(PGconn *connection,
                                const StatStateRecord &state,
                                const std::string &actor) {
    Json::Value details(Json::objectValue);
    details["currentVersion"] = Json::Int64(state.version);
    details["currentState"] = statStatePayload(connection, state, actor);
    return details;
}

drogon::HttpResponsePtr statStorageUnavailable() {
    return errorResponse(drogon::k503ServiceUnavailable,
                         "Stat ingestion storage is temporarily unavailable.",
                         "stat_ingestion_storage_unavailable",
                         true);
}
