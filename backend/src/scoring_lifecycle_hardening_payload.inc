Json::Value scoringStatePayload(PGconn *connection,
                                const std::string &leagueId,
                                const std::string &email,
                                const ScoringAccess &access,
                                int season,
                                int week,
                                const WeekRecord &record,
                                long long globalVersion,
                                long long standingsVersion) {
    Json::Value payload(Json::objectValue);
    payload["leagueId"] = leagueId;
    payload["season"] = season;
    payload["week"] = week;
    payload["version"] = Json::Int64(record.version);
    payload["weekVersion"] = Json::Int64(record.version);
    payload["globalVersion"] = Json::Int64(globalVersion);
    payload["standingsVersion"] = Json::Int64(standingsVersion);
    payload["status"] = record.status;
    payload["inputHash"] = record.inputHash;
    payload["scoringSettings"] = record.scoringSettings.isObject() && !record.scoringSettings.empty()
        ? record.scoringSettings
        : access.scoringSettings;
    payload["lineupSnapshot"] = record.lineupSnapshot;
    payload["scores"] = record.playerScores;
    payload["matchups"] = record.matchups;
    payload["standings"] = standingsPayload(connection, leagueId, season);
    payload["scoredAt"] = record.scoredAt;
    payload["finalizedAt"] = record.finalizedAt;
    payload["finalizedByEmail"] = record.finalizedBy;
    payload["commissioner"] = access.commissioner;
    payload["immutable"] = cff::scoring_lifecycle::finalizedStatus(record.status);
    payload["requestedByEmail"] = email;
    return payload;
}

Json::Value scoringConflictDetails(PGconn *connection,
                                   const std::string &leagueId,
                                   const std::string &email,
                                   const ScoringAccess &access,
                                   int season,
                                   int week,
                                   const WeekRecord &record,
                                   long long globalVersion,
                                   long long standingsVersion) {
    Json::Value details(Json::objectValue);
    details["currentVersion"] = Json::Int64(record.version);
    details["currentGlobalVersion"] = Json::Int64(globalVersion);
    details["currentStandingsVersion"] = Json::Int64(standingsVersion);
    details["state"] = scoringStatePayload(connection, leagueId, email, access, season, week,
                                           record, globalVersion, standingsVersion);
    return details;
}

drogon::HttpResponsePtr scoringStorageUnavailable() {
    return errorResponse(drogon::k503ServiceUnavailable,
                         "Scoring storage is temporarily unavailable.",
                         "scoring_storage_unavailable",
                         true);
}
