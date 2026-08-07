std::string waiverProcessingPeriod(const Json::Value &rules) {
    auto period = trim(rules.get("processingPeriod", "").asString());
    if (!period.empty()) return period;
    return trim(rules.get("claimDeadline", "").asString());
}

std::string generatedWaiverProcessingPeriod(const std::string &leagueId) {
    return "waiver-period-" + leagueId + "-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
}

std::string effectiveWaiverProcessingPeriod(PGconn *connection,
                                             const std::string &leagueId,
                                             const Json::Value &rules) {
    const auto configured = trim(rules.get("processingPeriod", "").asString());
    const auto deadlineCandidate = trim(rules.get("claimDeadline", "").asString());
    auto state = execute(connection,
        "SELECT COALESCE(current_processing_period, ''), COALESCE(last_processing_period, '') "
        "FROM waiver_states WHERE league_id = $1 LIMIT 1 FOR UPDATE",
        {leagueId});
    if (!tuplesOk(state) || PQntuples(state.get()) == 0) return configured.empty() ? deadlineCandidate : configured;

    auto current = cell(state.get(), 0, 0);
    const auto last = cell(state.get(), 0, 1);
    if (!configured.empty()) {
        if (current != configured) {
            auto updated = execute(connection,
                "UPDATE waiver_states SET current_processing_period = $2, updated_at = NOW() "
                "WHERE league_id = $1",
                {leagueId, configured});
            if (!commandOk(updated)) return current.empty() ? configured : current;
        }
        return configured;
    }

    if (current.empty()) {
        current = deadlineCandidate.empty() ? generatedWaiverProcessingPeriod(leagueId) : deadlineCandidate;
        auto updated = execute(connection,
            "UPDATE waiver_states SET current_processing_period = $2, updated_at = NOW() "
            "WHERE league_id = $1",
            {leagueId, current});
        if (!commandOk(updated)) return current;
        return current;
    }

    if (current == last
        && !cff::league_waiver::deadlinePassed(rules)
        && !deadlineCandidate.empty()
        && deadlineCandidate != current) {
        auto updated = execute(connection,
            "UPDATE waiver_states SET current_processing_period = $2, updated_at = NOW() "
            "WHERE league_id = $1",
            {leagueId, deadlineCandidate});
        if (commandOk(updated)) return deadlineCandidate;
    }
    return current;
}

std::string waiverFailureReason(const std::string &code) {
    if (code == "player_id_missing") return "The requested player is no longer identifiable.";
    if (code == "player_unavailable") return "The requested player was acquired by another manager.";
    if (code == "player_inactive") return "The requested player is no longer active in the player pool.";
    if (code == "player_ineligible") return "The requested player is not eligible for this league's roster configuration.";
    if (code == "player_locked") return "The requested player's game has started for the active week.";
    if (code == "drop_player_not_rostered") return "The conditional drop is no longer on this manager's roster.";
    if (code == "drop_player_locked") return "The conditional drop can no longer be moved because its lineup or game is locked.";
    if (code == "drop_player_conflict") return "The conditional drop changed while this claim was being processed.";
    if (code == "roster_full") return "The roster has no legal destination for this player after applying the conditional drop.";
    if (code == "waiver_period_expired") return "The claim belonged to an older waiver processing period and expired before award.";
    return code.empty() ? "" : "The waiver claim could not be completed under the latest league state.";
}

std::string phase5WaiverStatusForUi(const std::string &status) {
    if (status == "processed") return "Successful";
    if (status == "cancelled") return "Cancelled";
    if (status == "failed") return "Failed";
    if (status == "expired") return "Expired";
    return "Pending";
}

Json::Value phase5WaiverClaimsPayload(PGconn *connection,
                                      const std::string &leagueId,
                                      const std::string &email,
                                      bool commissioner) {
    auto result = execute(connection,
        "SELECT c.id, c.add_player_id, c.add_player_snapshot::text, "
        "COALESCE(c.drop_player_id, ''), c.status, "
        "COALESCE(to_char(c.created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), ''), "
        "lower(c.manager_email), COALESCE(w.priority, c.priority, 9999), c.claim_order, "
        "COALESCE(c.failure_code, ''), COALESCE(c.failure_reason, ''), "
        "COALESCE(to_char(c.processed_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), ''), "
        "COALESCE(c.resolved_by_email, ''), COALESCE(c.resolution_run_id, ''), "
        "COALESCE(c.processing_period, '') "
        "FROM waiver_claims c LEFT JOIN waiver_priorities w "
        "ON w.league_id = c.league_id AND lower(w.manager_email) = lower(c.manager_email) "
        "WHERE c.league_id = $1 AND ($3::boolean OR lower(c.manager_email) = lower($2)) "
        "ORDER BY CASE WHEN c.status = 'pending' THEN 0 ELSE 1 END, "
        "COALESCE(w.priority, c.priority, 9999), c.claim_order, c.created_at, c.id",
        {leagueId, email, commissioner ? "true" : "false"});
    Json::Value claims(Json::arrayValue);
    if (!tuplesOk(result)) return claims;
    for (int row = 0; row < PQntuples(result.get()); ++row) {
        Json::Value claim(Json::objectValue);
        claim["id"] = cell(result.get(), row, 0);
        auto player = jsonFromString(cell(result.get(), row, 2));
        if (!player.isObject()) player = Json::Value{Json::objectValue};
        player["id"] = cell(result.get(), row, 1);
        player["playerId"] = cell(result.get(), row, 1);
        claim["addPlayer"] = player;
        claim["addPlayerId"] = cell(result.get(), row, 1);
        claim["dropPlayerId"] = cell(result.get(), row, 3);
        claim["status"] = phase5WaiverStatusForUi(cell(result.get(), row, 4));
        claim["createdAt"] = cell(result.get(), row, 5);
        claim["submittedAt"] = cell(result.get(), row, 5);
        claim["managerEmail"] = canonicalEmail(cell(result.get(), row, 6));
        claim["teamId"] = canonicalEmail(cell(result.get(), row, 6));
        claim["priority"] = std::stoi(cell(result.get(), row, 7));
        claim["claimOrder"] = std::stoi(cell(result.get(), row, 8));
        claim["failureCode"] = cell(result.get(), row, 9);
        claim["failureReason"] = cell(result.get(), row, 10);
        claim["processedAt"] = cell(result.get(), row, 11);
        claim["resolvedByEmail"] = canonicalEmail(cell(result.get(), row, 12));
        claim["resolutionRunId"] = cell(result.get(), row, 13);
        claim["processingPeriod"] = cell(result.get(), row, 14);
        claims.append(claim);
    }
    return claims;
}

std::string lastCompletedWaiverPeriod(PGconn *connection,
                                      const std::string &leagueId) {
    auto result = execute(connection,
        "SELECT COALESCE(last_processing_period, '') FROM waiver_states "
        "WHERE league_id = $1 LIMIT 1 FOR UPDATE",
        {leagueId});
    return tuplesOk(result) && PQntuples(result.get()) > 0 ? cell(result.get(), 0, 0) : "";
}

bool completeWaiverProcessingPeriod(PGconn *connection,
                                    const std::string &leagueId,
                                    const std::string &period,
                                    const std::string &runId) {
    if (period.empty()) return true;
    return commandOk(execute(connection,
        "UPDATE waiver_states SET current_processing_period = $2, last_processing_period = $2, "
        "last_processed_at = NOW(), last_processing_run_id = $3, updated_at = NOW() "
        "WHERE league_id = $1",
        {leagueId, period, runId}));
}

bool prepareWaiverProcessingPeriod(PGconn *connection,
                                   const std::string &leagueId,
                                   const std::string &period,
                                   const std::string &actor,
                                   const std::string &runId) {
    if (period.empty()) return true;
    auto adopted = execute(connection,
        "UPDATE waiver_claims SET processing_period = $2, updated_at = NOW() "
        "WHERE league_id = $1 AND status = 'pending' AND processing_period = ''",
        {leagueId, period});
    if (!commandOk(adopted)) return false;

    auto expired = execute(connection,
        "UPDATE waiver_claims SET status = 'expired', failure_code = 'waiver_period_expired', "
        "failure_reason = $4, resolved_by_email = $3, resolution_run_id = $5, "
        "processed_at = NOW(), updated_at = NOW() "
        "WHERE league_id = $1 AND status = 'pending' AND processing_period <> $2 "
        "RETURNING id, lower(manager_email), add_player_id",
        {leagueId, period, actor, waiverFailureReason("waiver_period_expired"), runId});
    if (!tuplesOk(expired)) return false;
    for (int row = 0; row < PQntuples(expired.get()); ++row) {
        Json::Value metadata(Json::objectValue);
        metadata["claimId"] = cell(expired.get(), row, 0);
        metadata["playerId"] = cell(expired.get(), row, 2);
        metadata["processingPeriod"] = period;
        metadata["failureCode"] = "waiver_period_expired";
        if (!addWaiverTransactionRecord(connection, leagueId, canonicalEmail(cell(expired.get(), row, 1)),
                                        runId + "-expired-" + cell(expired.get(), row, 0),
                                        "Waiver Expired",
                                        waiverFailureReason("waiver_period_expired"),
                                        metadata)) return false;
    }
    return true;
}
