drogon::HttpResponsePtr scheduleStorageUnavailable() {
    return errorResponse(drogon::k503ServiceUnavailable,
                         "Schedule storage is temporarily unavailable.",
                         "schedule_storage_unavailable",
                         true);
}

Json::Value scheduleStatePayload(PGconn *connection,
                                 const std::string &leagueId,
                                 const std::string &email,
                                 const ScheduleAccess &access,
                                 int season,
                                 int week,
                                 const ScheduleRecord &scheduleRecordValue) {
    const auto weekState = weekStateRecord(connection, leagueId, season, week);
    const auto lineups = lineupStatesPayload(connection, leagueId, season, week);
    const auto now = isoNow();
    const auto deadlinePassed = cff::schedule_lineup_lifecycle::deadlinePassed(
        weekState.deadline, now);

    Json::Value payload(Json::objectValue);
    payload["leagueId"] = leagueId;
    payload["season"] = season;
    payload["week"] = week;
    payload["version"] = Json::Int64(scheduleRecordValue.version);
    payload["scheduleVersion"] = Json::Int64(scheduleRecordValue.version);
    payload["inputHash"] = scheduleRecordValue.inputHash;
    payload["weeks"] = scheduleRecordValue.weeks;
    payload["managerOrder"] = scheduleRecordValue.managerOrder;
    payload["schedule"] = scheduleRecordValue.schedule;
    payload["weekVersion"] = Json::Int64(weekState.version);
    payload["weekStatus"] = weekState.status;
    payload["lineupDeadline"] = weekState.deadline;
    payload["deadlinePassed"] = deadlinePassed;
    payload["lineups"] = lineups;
    payload["allLocked"] = allManagersLocked(connection, leagueId, season, week);
    payload["scoringStatus"] = scoringWeekStatus(connection, leagueId, season, week);
    payload["isCommissioner"] = access.commissioner;
    payload["accountEmail"] = email;
    payload["serverTime"] = now;

    bool ownLocked = false;
    std::string ownLockReason;
    for (const auto &lineup : lineups) {
        if (canonicalEmail(lineup.get("managerEmail", "").asString()) != email) continue;
        ownLocked = cff::schedule_lineup_lifecycle::lockedStatus(
            lineup.get("status", "open").asString());
        ownLockReason = lineup.get("lockReason", "").asString();
        payload["myLineup"] = lineup;
        break;
    }
    if (!payload.isMember("myLineup")) payload["myLineup"] = Json::Value{Json::objectValue};
    payload["lineupLocked"] = ownLocked;
    payload["lockReason"] = ownLockReason;
    payload["canEditLineup"] = access.member
        && !ownLocked
        && lower(weekState.status) != "finalized"
        && lower(scoringWeekStatus(connection, leagueId, season, week)) == "unscored";
    payload["canUnlockLineup"] = access.member
        && ownLocked
        && ownLockReason != "scoring"
        && !deadlinePassed
        && lower(scoringWeekStatus(connection, leagueId, season, week)) == "unscored";
    return payload;
}

Json::Value scheduleConflictDetails(PGconn *connection,
                                    const std::string &leagueId,
                                    const std::string &email,
                                    const ScheduleAccess &access,
                                    int season,
                                    int week) {
    auto current = scheduleRecord(connection, leagueId, season);
    Json::Value details(Json::objectValue);
    details["currentState"] = scheduleStatePayload(
        connection, leagueId, email, access, season, week, current);
    details["currentVersion"] = Json::Int64(current.version);
    return details;
}
