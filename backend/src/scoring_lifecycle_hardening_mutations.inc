struct ScoringContext {
    PgConnection connection;
    ScoringAccess access;
    WeekRecord week;
    long long globalVersion{0};
    long long standingsVersion{0};
};

std::optional<ScoringContext> openScoringContext(const std::string &leagueId,
                                                 const std::string &email,
                                                 int season,
                                                 int week) {
    auto connection = connectDb();
    if (!connection || !begin(connection.get()) || !lockScoringLeague(connection.get(), leagueId)) {
        if (connection) rollback(connection.get());
        return std::nullopt;
    }
    auto access = scoringAccess(connection.get(), leagueId, email);
    if (access.exists && (!ensureScoringState(connection.get(), leagueId)
        || !ensureWeekState(connection.get(), leagueId, season, week))) {
        rollback(connection.get());
        return std::nullopt;
    }
    const auto versions = scoringVersions(connection.get(), leagueId);
    ScoringContext context;
    context.connection = std::move(connection);
    context.access = access;
    context.week = weekRecord(context.connection.get(), leagueId, season, week);
    context.globalVersion = versions.first;
    context.standingsVersion = versions.second;
    return context;
}

bool persistPlayerScores(PGconn *connection,
                         const std::string &leagueId,
                         int season,
                         int week,
                         long long scoringVersion,
                         const std::string &hash,
                         const Json::Value &scores) {
    if (!commandOk(execute(connection,
        "DELETE FROM fantasy_player_scores WHERE league_id = $1 AND season = $2::int AND week = $3::int "
        "AND finalized_at IS NULL",
        {leagueId, std::to_string(season), std::to_string(week)}))) return false;
    for (const auto &score : scores) {
        if (!commandOk(execute(connection,
            "INSERT INTO fantasy_player_scores "
            "(league_id, manager_email, player_id, season, week, fantasy_points, stats, "
            "scoring_snapshot_hash, scoring_version, finalized_at, updated_at) "
            "VALUES ($1, $2, $3, $4::int, $5::int, $6::numeric, $7::jsonb, $8, $9::bigint, NULL, NOW()) "
            "ON CONFLICT (league_id, manager_email, player_id, season, week) DO UPDATE SET "
            "fantasy_points = EXCLUDED.fantasy_points, stats = EXCLUDED.stats, "
            "scoring_snapshot_hash = EXCLUDED.scoring_snapshot_hash, scoring_version = EXCLUDED.scoring_version, "
            "finalized_at = NULL, updated_at = NOW()",
            {leagueId,
             canonicalEmail(score.get("managerEmail", "").asString()),
             score.get("playerId", "").asString(),
             std::to_string(season), std::to_string(week),
             std::to_string(score.get("fantasyPoints", 0.0).asDouble()),
             jsonToString(score.get("stats", Json::Value{Json::objectValue})),
             hash, std::to_string(scoringVersion)}))) return false;
    }
    return true;
}

bool persistScoredMatchups(PGconn *connection,
                           const std::string &leagueId,
                           int season,
                           int week,
                           long long scoringVersion,
                           const std::string &hash,
                           const Json::Value &matchups) {
    if (!commandOk(execute(connection,
        "DELETE FROM league_matchups WHERE league_id = $1 AND week = $3::int "
        "AND (season = $2::int OR season = 0) AND status <> 'final'",
        {leagueId, std::to_string(season), std::to_string(week)}))) return false;
    for (const auto &matchup : matchups) {
        if (!commandOk(execute(connection,
            "INSERT INTO league_matchups "
            "(id, league_id, season, week, home_manager_email, away_manager_email, home_score, away_score, "
            "status, scoring_snapshot_hash, scoring_version, finalized_at, created_at, updated_at) "
            "VALUES ($1, $2, $3::int, $4::int, $5, NULLIF($6, ''), $7::numeric, $8::numeric, "
            "'scheduled', $9, $10::bigint, NULL, NOW(), NOW()) "
            "ON CONFLICT (id) DO UPDATE SET season = EXCLUDED.season, week = EXCLUDED.week, "
            "home_manager_email = EXCLUDED.home_manager_email, away_manager_email = EXCLUDED.away_manager_email, "
            "home_score = EXCLUDED.home_score, away_score = EXCLUDED.away_score, status = 'scheduled', "
            "scoring_snapshot_hash = EXCLUDED.scoring_snapshot_hash, scoring_version = EXCLUDED.scoring_version, "
            "finalized_at = NULL, updated_at = NOW()",
            {matchup.get("id", "").asString(), leagueId, std::to_string(season), std::to_string(week),
             canonicalEmail(matchup.get("homeManager", "").asString()),
             canonicalEmail(matchup.get("awayManager", "").asString()),
             std::to_string(matchup.get("homeScore", 0.0).asDouble()),
             std::to_string(matchup.get("awayScore", 0.0).asDouble()),
             hash, std::to_string(scoringVersion)}))) return false;
    }
    return true;
}

Json::Value scoredMatchups(const Json::Value &members,
                           const Json::Value &existing,
                           const std::string &leagueId,
                           int season,
                           int week,
                           const std::unordered_map<std::string, double> &managerTotals) {
    Json::Value result(Json::arrayValue);
    if (existing.isArray() && !existing.empty()) {
        for (const auto &source : existing) {
            Json::Value matchup = source;
            const auto home = canonicalEmail(matchup.get("homeManager", "").asString());
            const auto away = canonicalEmail(matchup.get("awayManager", "").asString());
            matchup["leagueId"] = leagueId;
            matchup["season"] = season;
            matchup["week"] = week;
            matchup["homeManager"] = home;
            matchup["awayManager"] = away;
            matchup["homeScore"] = managerTotals.count(home) ? managerTotals.at(home) : 0.0;
            matchup["awayScore"] = away.empty() || !managerTotals.count(away) ? 0.0 : managerTotals.at(away);
            matchup["status"] = "scheduled";
            matchup["finalizedAt"] = "";
            result.append(matchup);
        }
        return result;
    }
    result = cff::league_schedule::buildMatchups(
        members,
        leagueId,
        week,
        [&managerTotals](const std::string &managerEmail) {
            const auto email = canonicalEmail(managerEmail);
            const auto found = managerTotals.find(email);
            return found == managerTotals.end() ? 0.0 : found->second;
        });
    for (auto &matchup : result) {
        matchup["season"] = season;
        matchup["status"] = "scheduled";
        matchup["finalizedAt"] = "";
    }
    return result;
}

bool persistScoredWeek(PGconn *connection,
                       const std::string &leagueId,
                       int season,
                       int week,
                       long long weekVersion,
                       const std::string &hash,
                       const Json::Value &settings,
                       const Json::Value &lineup,
                       const Json::Value &scores,
                       const Json::Value &matchups) {
    auto result = execute(connection,
        "UPDATE scoring_week_states SET version = $4::bigint, status = 'scored', input_hash = $5, "
        "scoring_settings_snapshot = $6::jsonb, lineup_snapshot = $7::jsonb, "
        "player_scores_snapshot = $8::jsonb, matchup_snapshot = $9::jsonb, "
        "scored_at = NOW(), finalized_at = NULL, finalized_by_email = '', updated_at = NOW() "
        "WHERE league_id = $1 AND season = $2::int AND week = $3::int AND status <> 'final'",
        {leagueId, std::to_string(season), std::to_string(week), std::to_string(weekVersion),
         hash, jsonToString(settings), jsonToString(lineup), jsonToString(scores), jsonToString(matchups)});
    return commandOk(result) && std::string{PQcmdTuples(result.get())} == "1";
}

bool persistFinalWeek(PGconn *connection,
                      const std::string &leagueId,
                      const std::string &email,
                      int season,
                      int week,
                      long long weekVersion) {
    auto matchupUpdate = execute(connection,
        "UPDATE league_matchups SET status = 'final', finalized_at = COALESCE(finalized_at, NOW()), updated_at = NOW() "
        "WHERE league_id = $1 AND season = $2::int AND week = $3::int AND status <> 'final'",
        {leagueId, std::to_string(season), std::to_string(week)});
    auto scoreUpdate = execute(connection,
        "UPDATE fantasy_player_scores SET finalized_at = COALESCE(finalized_at, NOW()), updated_at = NOW() "
        "WHERE league_id = $1 AND season = $2::int AND week = $3::int",
        {leagueId, std::to_string(season), std::to_string(week)});
    auto weekUpdate = execute(connection,
        "UPDATE scoring_week_states SET version = $4::bigint, status = 'final', "
        "matchup_snapshot = (SELECT COALESCE(jsonb_agg(jsonb_build_object("
        "'id', m.id, 'leagueId', m.league_id, 'season', m.season, 'week', m.week, "
        "'homeManager', lower(m.home_manager_email), 'awayManager', lower(COALESCE(m.away_manager_email, '')), "
        "'homeScore', m.home_score, 'awayScore', m.away_score, 'status', m.status, "
        "'finalizedAt', COALESCE(to_char(m.finalized_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), '')"
        ") ORDER BY m.id), '[]'::jsonb) FROM league_matchups m "
        "WHERE m.league_id = $1 AND m.season = $2::int AND m.week = $3::int), "
        "finalized_at = COALESCE(finalized_at, NOW()), finalized_by_email = $5, updated_at = NOW() "
        "WHERE league_id = $1 AND season = $2::int AND week = $3::int AND status = 'scored'",
        {leagueId, std::to_string(season), std::to_string(week), std::to_string(weekVersion), email});
    return commandOk(matchupUpdate)
        && commandOk(scoreUpdate)
        && commandOk(weekUpdate)
        && std::string{PQcmdTuples(weekUpdate.get())} == "1";
}

drogon::HttpResponsePtr getScoringState(const std::string &leagueId,
                                        const std::string &email,
                                        int season,
                                        int week) {
    auto context = openScoringContext(leagueId, email, season, week);
    if (!context) return scoringStorageUnavailable();
    if (!context->access.exists) {
        rollback(context->connection.get());
        return errorResponse(drogon::k404NotFound, "League not found.", "league_not_found");
    }
    if (!context->access.member) {
        rollback(context->connection.get());
        return errorResponse(drogon::k403Forbidden,
                             "Active league membership is required.",
                             "league_membership_required");
    }
    auto payload = scoringStatePayload(context->connection.get(), leagueId, email, context->access,
                                       season, week, context->week,
                                       context->globalVersion, context->standingsVersion);
    if (!commit(context->connection.get())) {
        rollback(context->connection.get());
        return scoringStorageUnavailable();
    }
    return jsonResponse(payload);
}

drogon::HttpResponsePtr getStandingsState(const std::string &leagueId,
                                          const std::string &email,
                                          int season) {
    auto context = openScoringContext(leagueId, email, season, 1);
    if (!context) return scoringStorageUnavailable();
    if (!context->access.exists) {
        rollback(context->connection.get());
        return errorResponse(drogon::k404NotFound, "League not found.", "league_not_found");
    }
    if (!context->access.member) {
        rollback(context->connection.get());
        return errorResponse(drogon::k403Forbidden,
                             "Active league membership is required.",
                             "league_membership_required");
    }
    Json::Value payload(Json::objectValue);
    payload["leagueId"] = leagueId;
    payload["season"] = season;
    payload["version"] = Json::Int64(context->standingsVersion);
    payload["standingsVersion"] = Json::Int64(context->standingsVersion);
    payload["standings"] = standingsPayload(context->connection.get(), leagueId, season);
    if (!commit(context->connection.get())) {
        rollback(context->connection.get());
        return scoringStorageUnavailable();
    }
    return jsonResponse(payload);
}

drogon::HttpResponsePtr scoreWeek(const drogon::HttpRequestPtr &request,
                                  const std::string &leagueId,
                                  const std::string &email,
                                  int season,
                                  int week,
                                  bool legacy) {
    const auto body = request->getJsonObject();
    const Json::Value requestBody = body && body->isObject() ? *body : Json::Value{Json::objectValue};
    const auto key = operationKey(request);
    if (!legacy && key.empty()) {
        return errorResponse(drogon::k400BadRequest,
                             "An Idempotency-Key is required for scoring mutations.",
                             "idempotency_key_required");
    }
    auto context = openScoringContext(leagueId, email, season, week);
    if (!context) return scoringStorageUnavailable();
    if (!context->access.exists) {
        rollback(context->connection.get());
        return errorResponse(drogon::k404NotFound, "League not found.", "league_not_found");
    }
    if (!context->access.commissioner) {
        rollback(context->connection.get());
        return errorResponse(drogon::k403Forbidden,
                             "Only the league commissioner can score a week.",
                             "commissioner_required");
    }
    if (const auto replay = scoringOperationReplay(
            context->connection.get(), leagueId, email, key, "score")) {
        if (!(*replay)["operationTypeMatches"].asBool()) {
            rollback(context->connection.get());
            return errorResponse(drogon::k409Conflict,
                                 "This idempotency key was already used for another scoring action.",
                                 "idempotency_key_conflict");
        }
        auto payload = *replay;
        payload.removeMember("operationTypeMatches");
        payload.removeMember("storedOperationType");
        if (!commit(context->connection.get())) {
            rollback(context->connection.get());
            return scoringStorageUnavailable();
        }
        return jsonResponse(payload);
    }
    if (cff::scoring_lifecycle::finalizedStatus(context->week.status)) {
        const auto details = scoringConflictDetails(context->connection.get(), leagueId, email,
            context->access, season, week, context->week,
            context->globalVersion, context->standingsVersion);
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict,
                             "Finalized weeks are immutable.",
                             "week_finalized",
                             false,
                             details);
    }
    if (!cff::scoring_lifecycle::expectedVersionMatches(context->week.version, requestBody, !legacy)) {
        const auto details = scoringConflictDetails(context->connection.get(), leagueId, email,
            context->access, season, week, context->week,
            context->globalVersion, context->standingsVersion);
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict,
                             "Scoring state changed. Load the latest week before retrying.",
                             "scoring_state_conflict",
                             false,
                             details);
    }

    const auto members = activeMembersPayload(context->connection.get(), leagueId);
    const auto inputs = scoreInputs(context->connection.get(), leagueId, season, week,
                                    context->access.scoringSettings);
    const auto errors = lineupErrorsPayload(members, inputs.lineup, context->access.rosterRules);
    if (!errors.empty()) {
        Json::Value details(Json::objectValue);
        details["lineupErrors"] = errors;
        details["state"] = scoringStatePayload(context->connection.get(), leagueId, email,
            context->access, season, week, context->week,
            context->globalVersion, context->standingsVersion);
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict,
                             "One or more active managers have an invalid lineup.",
                             "invalid_lineup",
                             false,
                             details);
    }

    Json::Value hashInput(Json::objectValue);
    hashInput["leagueId"] = leagueId;
    hashInput["season"] = season;
    hashInput["week"] = week;
    hashInput["scoringSettings"] = context->access.scoringSettings;
    hashInput["lineup"] = inputs.lineup;
    hashInput["scores"] = inputs.playerScores;
    const auto hash = inputHash(context->connection.get(), hashInput);
    if (hash.empty()) {
        rollback(context->connection.get());
        return scoringStorageUnavailable();
    }
    if (context->week.status == "scored" && context->week.inputHash == hash) {
        auto payload = scoringStatePayload(context->connection.get(), leagueId, email, context->access,
            season, week, context->week, context->globalVersion, context->standingsVersion);
        payload["action"] = "score";
        payload["unchanged"] = true;
        payload["operationKey"] = key;
        if (!recordScoringOperation(context->connection.get(), leagueId, email, key, "score",
                                    season, week, context->week.version,
                                    context->standingsVersion, payload)
            || !commit(context->connection.get())) {
            rollback(context->connection.get());
            return scoringStorageUnavailable();
        }
        return jsonResponse(payload);
    }

    auto matchups = scoredMatchups(members,
        existingWeekMatchups(context->connection.get(), leagueId, season, week),
        leagueId, season, week, inputs.managerTotals);
    if (matchups.empty()) {
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict,
                             "At least two active managers are required to score a week.",
                             "matchups_unavailable");
    }
    const auto nextWeekVersion = context->week.version + 1;
    if (!persistPlayerScores(context->connection.get(), leagueId, season, week,
                             nextWeekVersion, hash, inputs.playerScores)
        || !persistScoredMatchups(context->connection.get(), leagueId, season, week,
                                  nextWeekVersion, hash, matchups)
        || !persistScoredWeek(context->connection.get(), leagueId, season, week,
                              nextWeekVersion, hash, context->access.scoringSettings,
                              inputs.lineup, inputs.playerScores, matchups)) {
        rollback(context->connection.get());
        return scoringStorageUnavailable();
    }
    const auto versions = advanceScoringVersions(context->connection.get(), leagueId, false);
    if (versions.first < 0) {
        rollback(context->connection.get());
        return scoringStorageUnavailable();
    }
    Json::Value metadata(Json::objectValue);
    metadata["season"] = season;
    metadata["week"] = week;
    metadata["inputHash"] = hash;
    metadata["weekVersion"] = Json::Int64(nextWeekVersion);
    if (!addScoringTransaction(context->connection.get(), leagueId, email, key,
                               "Scoring", "Calculated week " + std::to_string(week) + " fantasy scores",
                               metadata)) {
        rollback(context->connection.get());
        return scoringStorageUnavailable();
    }
    const auto saved = weekRecord(context->connection.get(), leagueId, season, week);
    auto payload = scoringStatePayload(context->connection.get(), leagueId, email, context->access,
        season, week, saved, versions.first, versions.second);
    payload["action"] = "score";
    payload["operationKey"] = key;
    if (!recordScoringOperation(context->connection.get(), leagueId, email, key, "score",
                                season, week, nextWeekVersion, versions.second, payload)
        || !commit(context->connection.get())) {
        rollback(context->connection.get());
        return scoringStorageUnavailable();
    }
    return jsonResponse(payload);
}

drogon::HttpResponsePtr finalizeWeek(const drogon::HttpRequestPtr &request,
                                     const std::string &leagueId,
                                     const std::string &email,
                                     int season,
                                     int week,
                                     bool legacy) {
    const auto body = request->getJsonObject();
    const Json::Value requestBody = body && body->isObject() ? *body : Json::Value{Json::objectValue};
    const auto key = operationKey(request);
    if (!legacy && key.empty()) {
        return errorResponse(drogon::k400BadRequest,
                             "An Idempotency-Key is required for scoring mutations.",
                             "idempotency_key_required");
    }
    auto context = openScoringContext(leagueId, email, season, week);
    if (!context) return scoringStorageUnavailable();
    if (!context->access.exists) {
        rollback(context->connection.get());
        return errorResponse(drogon::k404NotFound, "League not found.", "league_not_found");
    }
    if (!context->access.commissioner) {
        rollback(context->connection.get());
        return errorResponse(drogon::k403Forbidden,
                             "Only the league commissioner can finalize a week.",
                             "commissioner_required");
    }
    if (const auto replay = scoringOperationReplay(
            context->connection.get(), leagueId, email, key, "finalize")) {
        if (!(*replay)["operationTypeMatches"].asBool()) {
            rollback(context->connection.get());
            return errorResponse(drogon::k409Conflict,
                                 "This idempotency key was already used for another scoring action.",
                                 "idempotency_key_conflict");
        }
        auto payload = *replay;
        payload.removeMember("operationTypeMatches");
        payload.removeMember("storedOperationType");
        if (!commit(context->connection.get())) {
            rollback(context->connection.get());
            return scoringStorageUnavailable();
        }
        return legacy ? jsonResponse(payload["matchups"]) : jsonResponse(payload);
    }
    if (cff::scoring_lifecycle::finalizedStatus(context->week.status)) {
        auto payload = scoringStatePayload(context->connection.get(), leagueId, email, context->access,
            season, week, context->week, context->globalVersion, context->standingsVersion);
        payload["action"] = "finalize";
        payload["alreadyFinal"] = true;
        payload["operationKey"] = key;
        if (!recordScoringOperation(context->connection.get(), leagueId, email, key, "finalize",
                                    season, week, context->week.version,
                                    context->standingsVersion, payload)
            || !commit(context->connection.get())) {
            rollback(context->connection.get());
            return scoringStorageUnavailable();
        }
        return legacy ? jsonResponse(payload["matchups"]) : jsonResponse(payload);
    }
    if (!cff::scoring_lifecycle::expectedVersionMatches(context->week.version, requestBody, !legacy)) {
        const auto details = scoringConflictDetails(context->connection.get(), leagueId, email,
            context->access, season, week, context->week,
            context->globalVersion, context->standingsVersion);
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict,
                             "Scoring state changed. Load the latest week before retrying.",
                             "scoring_state_conflict",
                             false,
                             details);
    }
    if (context->week.status != "scored") {
        const auto details = scoringConflictDetails(context->connection.get(), leagueId, email,
            context->access, season, week, context->week,
            context->globalVersion, context->standingsVersion);
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict,
                             "Score the week before finalizing it.",
                             "week_not_scored",
                             false,
                             details);
    }

    const auto nextWeekVersion = context->week.version + 1;
    if (!persistFinalWeek(context->connection.get(), leagueId, email, season, week, nextWeekVersion)) {
        rollback(context->connection.get());
        return scoringStorageUnavailable();
    }
    const auto versions = advanceScoringVersions(context->connection.get(), leagueId, true);
    if (versions.first < 0 || versions.second < 0) {
        rollback(context->connection.get());
        return scoringStorageUnavailable();
    }
    const auto members = activeMembersPayload(context->connection.get(), leagueId);
    const auto finalMatchups = seasonFinalMatchups(context->connection.get(), leagueId, season);
    const auto standings = cff::scoring_lifecycle::standingsFromFinalMatchups(members, finalMatchups, season);
    if (!replaceStandings(context->connection.get(), leagueId, season, versions.second, standings)) {
        rollback(context->connection.get());
        return scoringStorageUnavailable();
    }
    Json::Value metadata(Json::objectValue);
    metadata["season"] = season;
    metadata["week"] = week;
    metadata["weekVersion"] = Json::Int64(nextWeekVersion);
    metadata["standingsVersion"] = Json::Int64(versions.second);
    if (!addScoringTransaction(context->connection.get(), leagueId, email, key,
                               "Scoring Finalized", "Finalized week " + std::to_string(week),
                               metadata)) {
        rollback(context->connection.get());
        return scoringStorageUnavailable();
    }
    const auto saved = weekRecord(context->connection.get(), leagueId, season, week);
    auto payload = scoringStatePayload(context->connection.get(), leagueId, email, context->access,
        season, week, saved, versions.first, versions.second);
    payload["action"] = "finalize";
    payload["operationKey"] = key;
    if (!recordScoringOperation(context->connection.get(), leagueId, email, key, "finalize",
                                season, week, nextWeekVersion, versions.second, payload)
        || !commit(context->connection.get())) {
        rollback(context->connection.get());
        return scoringStorageUnavailable();
    }
    return legacy ? jsonResponse(payload["matchups"]) : jsonResponse(payload);
}

drogon::HttpResponsePtr dispatchScoringTransaction(const drogon::HttpRequestPtr &request,
                                                    const std::string &leagueId,
                                                    const std::string &email) {
    const auto body = request->getJsonObject();
    if (!body || !body->isObject()) {
        return errorResponse(drogon::k400BadRequest,
                             "A scoring action is required.",
                             "scoring_payload_required");
    }
    const auto action = lower(trim(body->get("action", "").asString()));
    const auto season = body->isMember("season")
        ? positiveInt((*body)["season"], currentSeasonYear())
        : currentSeasonYear();
    const auto week = body->isMember("week") ? positiveInt((*body)["week"], 1) : 1;
    if (action == "score") return scoreWeek(request, leagueId, email, season, week, false);
    if (action == "finalize") return finalizeWeek(request, leagueId, email, season, week, false);
    return errorResponse(drogon::k400BadRequest,
                         "Supported scoring actions are score and finalize.",
                         "invalid_scoring_action");
}
