struct ScheduleContext {
    PgConnection connection;
    ScheduleAccess access;
    ScheduleStateRecord state;
};

std::optional<ScheduleContext> openScheduleContext(const std::string &leagueId,
                                                   const std::string &email,
                                                   int season) {
    auto connection = connectDb();
    if (!connection || !begin(connection.get()) || !lockScheduleLeague(connection.get(), leagueId)) {
        if (connection) rollback(connection.get());
        return std::nullopt;
    }
    const auto access = scheduleAccess(connection.get(), leagueId, email);
    if (access.exists && (!ensureScheduleState(connection.get(), leagueId, season)
        || !refreshExpiredDeadlines(connection.get(), leagueId, season))) {
        rollback(connection.get());
        return std::nullopt;
    }
    ScheduleContext context;
    context.connection = std::move(connection);
    context.access = access;
    context.state = scheduleStateRecord(context.connection.get(), leagueId);
    return context;
}

bool expectedVersionMatches(long long currentVersion,
                            const Json::Value &body,
                            bool required) {
    if (!body.isObject() || !body.isMember("expectedVersion")) return !required;
    return int64Value(body["expectedVersion"], -1) == currentVersion;
}

bool persistSchedule(PGconn *connection,
                     const std::string &leagueId,
                     const std::string &email,
                     int season,
                     int weeks,
                     long long version,
                     const std::string &hash,
                     const Json::Value &matchups) {
    if (!commandOk(execute(connection,
        "DELETE FROM league_matchups WHERE league_id = $1 AND (season = $2::int OR season = 0) "
        "AND status <> 'final'",
        {leagueId, std::to_string(season)}))) return false;

    for (const auto &matchup : matchups) {
        if (!commandOk(execute(connection,
            "INSERT INTO league_matchups "
            "(id, league_id, season, week, matchup_key, schedule_slot, schedule_version, "
            "home_manager_email, away_manager_email, home_score, away_score, status, created_at, updated_at) "
            "VALUES ($1, $2, $3::int, $4::int, $5, $6::int, $7::bigint, $8, NULLIF($9, ''), 0, 0, 'scheduled', NOW(), NOW()) "
            "ON CONFLICT (id) DO UPDATE SET season = EXCLUDED.season, week = EXCLUDED.week, "
            "matchup_key = EXCLUDED.matchup_key, schedule_slot = EXCLUDED.schedule_slot, "
            "schedule_version = EXCLUDED.schedule_version, home_manager_email = EXCLUDED.home_manager_email, "
            "away_manager_email = EXCLUDED.away_manager_email, updated_at = NOW()",
            {matchup.get("id", "").asString(), leagueId, std::to_string(season),
             std::to_string(matchup.get("week", 1).asInt()),
             matchup.get("matchupKey", "").asString(),
             std::to_string(matchup.get("scheduleSlot", 0).asInt()),
             std::to_string(version),
             canonicalEmail(matchup.get("homeManager", "").asString()),
             canonicalEmail(matchup.get("awayManager", "").asString())}))) return false;
    }

    if (!commandOk(execute(connection,
        "DELETE FROM league_week_controls WHERE league_id = $1 AND season = $2::int",
        {leagueId, std::to_string(season)}))) return false;
    for (int week = 1; week <= weeks; ++week) {
        if (!commandOk(execute(connection,
            "INSERT INTO league_week_controls "
            "(league_id, season, week, schedule_version, lineup_version, status, updated_at) "
            "VALUES ($1, $2::int, $3::int, $4::bigint, 0, 'open', NOW())",
            {leagueId, std::to_string(season), std::to_string(week), std::to_string(version)}))) return false;
    }

    auto update = execute(connection,
        "UPDATE league_schedule_states SET season = $2::int, weeks = $3::int, current_week = 1, "
        "version = $4::bigint, schedule_hash = $5, generated_by_email = $6, "
        "generated_at = NOW(), updated_at = NOW() WHERE league_id = $1",
        {leagueId, std::to_string(season), std::to_string(weeks), std::to_string(version), hash, email});
    if (!commandOk(update) || std::string{PQcmdTuples(update.get())} != "1") return false;

    return commandOk(execute(connection,
        "INSERT INTO transactions (id, league_id, manager_email, transaction_type, summary, metadata, created_at) "
        "VALUES ('schedule-' || md5($1 || ':' || $4), $1, $2, 'Schedule', "
        "'Generated ' || $3 || '-week deterministic season schedule', "
        "jsonb_build_object('season', $5::int, 'scheduleVersion', $4::bigint, 'scheduleHash', $6), NOW())",
        {leagueId, email, std::to_string(weeks), std::to_string(version), std::to_string(season), hash}));
}

drogon::HttpResponsePtr getScheduleState(const std::string &leagueId,
                                         const std::string &email,
                                         int season) {
    auto context = openScheduleContext(leagueId, email, season);
    if (!context) return scheduleStorageUnavailable();
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
    context->state = scheduleStateRecord(context->connection.get(), leagueId);
    auto payload = scheduleStatePayload(context->connection.get(), leagueId, email,
                                        context->access, context->state);
    if (!commit(context->connection.get())) {
        rollback(context->connection.get());
        return scheduleStorageUnavailable();
    }
    return jsonResponse(payload);
}

drogon::HttpResponsePtr generateSchedule(const drogon::HttpRequestPtr &request,
                                         const std::string &leagueId,
                                         const std::string &email,
                                         bool legacy,
                                         int legacyWeeks = 0) {
    const auto bodyPointer = request->getJsonObject();
    Json::Value body = bodyPointer && bodyPointer->isObject() ? *bodyPointer : Json::Value{Json::objectValue};
    const auto season = body.isMember("season") ? positiveInt(body["season"], currentSeasonYear()) : currentSeasonYear();
    const auto weeks = std::clamp(legacyWeeks > 0 ? legacyWeeks : positiveInt(body["weeks"], 12), 1, 18);
    const auto key = operationKey(request);
    if (!legacy && key.empty()) {
        return errorResponse(drogon::k400BadRequest,
                             "An Idempotency-Key is required for schedule mutations.",
                             "idempotency_key_required");
    }
    auto context = openScheduleContext(leagueId, email, season);
    if (!context) return scheduleStorageUnavailable();
    if (!context->access.exists) {
        rollback(context->connection.get());
        return errorResponse(drogon::k404NotFound, "League not found.", "league_not_found");
    }
    if (!context->access.commissioner) {
        rollback(context->connection.get());
        return errorResponse(drogon::k403Forbidden,
                             "Only the league commissioner can generate the schedule.",
                             "commissioner_required");
    }
    if (const auto replay = scheduleOperationReplay(context->connection.get(), leagueId, email, key, "generate")) {
        if (!(*replay)["operationTypeMatches"].asBool()) {
            rollback(context->connection.get());
            return errorResponse(drogon::k409Conflict,
                                 "This idempotency key was already used for another schedule action.",
                                 "idempotency_key_conflict");
        }
        auto payload = *replay;
        payload.removeMember("operationTypeMatches");
        payload.removeMember("storedOperationType");
        if (!commit(context->connection.get())) {
            rollback(context->connection.get());
            return scheduleStorageUnavailable();
        }
        return legacy ? jsonResponse(payload["matchups"]) : jsonResponse(payload);
    }
    if (!expectedVersionMatches(context->state.version, body, !legacy)) {
        auto details = scheduleConflictDetails(context->connection.get(), leagueId, email,
                                               context->access, context->state);
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict,
                             "Schedule state changed. Load the latest schedule before retrying.",
                             "schedule_state_conflict",
                             false,
                             details);
    }
    if (scheduleMutationBlocked(context->connection.get(), leagueId, season)) {
        auto details = scheduleConflictDetails(context->connection.get(), leagueId, email,
                                               context->access, context->state);
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict,
                             "The schedule cannot be regenerated after a lineup is locked or scoring begins.",
                             "schedule_locked",
                             false,
                             details);
    }
    const auto members = activeMembersPayload(context->connection.get(), leagueId);
    if (members.size() < 2) {
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict,
                             "At least two active managers are required.",
                             "insufficient_active_managers");
    }
    const auto matchups = cff::schedule_lifecycle::buildDeterministicSeason(
        members, leagueId, season, weeks);
    const auto hash = cff::schedule_lifecycle::scheduleFingerprint(matchups);
    if (context->state.season == season && context->state.weeks == weeks
        && context->state.scheduleHash == hash && !hash.empty()) {
        auto payload = scheduleStatePayload(context->connection.get(), leagueId, email,
                                            context->access, context->state);
        payload["unchanged"] = true;
        if (!storeScheduleOperation(context->connection.get(), leagueId, email, key,
                                    "generate", context->state.version, payload)
            || !commit(context->connection.get())) {
            rollback(context->connection.get());
            return scheduleStorageUnavailable();
        }
        return legacy ? jsonResponse(payload["matchups"]) : jsonResponse(payload);
    }

    const auto nextVersion = context->state.version + 1;
    if (!persistSchedule(context->connection.get(), leagueId, email, season, weeks,
                         nextVersion, hash, matchups)) {
        rollback(context->connection.get());
        return scheduleStorageUnavailable();
    }
    context->state = scheduleStateRecord(context->connection.get(), leagueId);
    auto payload = scheduleStatePayload(context->connection.get(), leagueId, email,
                                        context->access, context->state);
    payload["generated"] = true;
    if (!storeScheduleOperation(context->connection.get(), leagueId, email, key,
                                "generate", nextVersion, payload)
        || !commit(context->connection.get())) {
        rollback(context->connection.get());
        return scheduleStorageUnavailable();
    }
    return legacy ? jsonResponse(payload["matchups"]) : jsonResponse(payload, drogon::k201Created);
}

std::optional<std::string> weekControlStatus(PGconn *connection,
                                             const std::string &leagueId,
                                             int season,
                                             int week) {
    auto result = execute(connection,
        "SELECT status FROM league_week_controls WHERE league_id = $1 AND season = $2::int AND week = $3::int",
        {leagueId, std::to_string(season), std::to_string(week)});
    if (!tuplesOk(result) || PQntuples(result.get()) == 0) return std::nullopt;
    return cell(result.get(), 0, 0);
}

drogon::HttpResponsePtr mutateLineupControl(const drogon::HttpRequestPtr &request,
                                            const std::string &leagueId,
                                            const std::string &email,
                                            int week,
                                            const std::string &action,
                                            bool legacy = false) {
    const auto bodyPointer = request->getJsonObject();
    const Json::Value body = bodyPointer && bodyPointer->isObject() ? *bodyPointer : Json::Value{Json::objectValue};
    const auto season = body.isMember("season") ? positiveInt(body["season"], currentSeasonYear()) : requestSeason(request);
    const auto key = operationKey(request);
    if (!legacy && key.empty()) {
        return errorResponse(drogon::k400BadRequest,
                             "An Idempotency-Key is required for lineup mutations.",
                             "idempotency_key_required");
    }
    auto context = openScheduleContext(leagueId, email, season);
    if (!context) return scheduleStorageUnavailable();
    if (!context->access.exists) {
        rollback(context->connection.get());
        return errorResponse(drogon::k404NotFound, "League not found.", "league_not_found");
    }
    if (!context->access.commissioner) {
        rollback(context->connection.get());
        return errorResponse(drogon::k403Forbidden,
                             "Only the league commissioner can change lineup controls.",
                             "commissioner_required");
    }
    if (week < 1 || week > context->state.weeks) {
        rollback(context->connection.get());
        return errorResponse(drogon::k400BadRequest, "Week is outside the generated schedule.", "invalid_week");
    }
    if (const auto replay = scheduleOperationReplay(context->connection.get(), leagueId, email, key, action)) {
        if (!(*replay)["operationTypeMatches"].asBool()) {
            rollback(context->connection.get());
            return errorResponse(drogon::k409Conflict,
                                 "This idempotency key was already used for another schedule action.",
                                 "idempotency_key_conflict");
        }
        auto payload = *replay;
        payload.removeMember("operationTypeMatches");
        payload.removeMember("storedOperationType");
        if (!commit(context->connection.get())) {
            rollback(context->connection.get());
            return scheduleStorageUnavailable();
        }
        return jsonResponse(payload);
    }
    if (!expectedVersionMatches(context->state.version, body, !legacy)) {
        auto details = scheduleConflictDetails(context->connection.get(), leagueId, email,
                                               context->access, context->state);
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict,
                             "Schedule state changed. Load the latest state before retrying.",
                             "schedule_state_conflict",
                             false,
                             details);
    }
    const auto status = weekControlStatus(context->connection.get(), leagueId, context->state.season, week);
    if (!status) {
        rollback(context->connection.get());
        return errorResponse(drogon::k404NotFound, "Week control was not found.", "week_control_not_found");
    }

    bool changed = false;
    if (action == "lock") {
        if (*status == "finalized") {
            rollback(context->connection.get());
            return errorResponse(drogon::k409Conflict, "Finalized lineups are immutable.", "lineup_finalized");
        }
        if (*status == "open") {
            const auto lineup = currentLineupSnapshot(context->connection.get(), leagueId);
            auto update = execute(context->connection.get(),
                "UPDATE league_week_controls SET status = 'locked', lineup_version = lineup_version + 1, "
                "lineup_snapshot = $4::jsonb, lineup_hash = md5(($4::jsonb)::text), locked_at = NOW(), "
                "locked_by_email = $5, lock_reason = 'commissioner', updated_at = NOW() "
                "WHERE league_id = $1 AND season = $2::int AND week = $3::int AND status = 'open'",
                {leagueId, std::to_string(context->state.season), std::to_string(week), jsonToString(lineup), email});
            changed = commandOk(update) && std::string{PQcmdTuples(update.get())} == "1";
        }
    } else if (action == "unlock") {
        const auto scoreStatus = scoringStatus(context->connection.get(), leagueId, context->state.season, week);
        if (!cff::schedule_lifecycle::canUnlockLineup(*status, scoreStatus,
                matchupFinal(context->connection.get(), leagueId, context->state.season, week))) {
            rollback(context->connection.get());
            return errorResponse(drogon::k409Conflict,
                                 "A lineup cannot be unlocked after scoring or finalization.",
                                 "lineup_unlock_forbidden");
        }
        auto update = execute(context->connection.get(),
            "UPDATE league_week_controls SET status = 'open', unlocked_at = NOW(), "
            "unlocked_by_email = $4, updated_at = NOW() "
            "WHERE league_id = $1 AND season = $2::int AND week = $3::int AND status = 'locked'",
            {leagueId, std::to_string(context->state.season), std::to_string(week), email});
        changed = commandOk(update) && std::string{PQcmdTuples(update.get())} == "1";
    } else if (action == "set_deadline") {
        const auto deadline = trim(body.get("lineupDeadline", "").asString());
        if (deadline.empty()) {
            rollback(context->connection.get());
            return errorResponse(drogon::k400BadRequest,
                                 "lineupDeadline is required.",
                                 "lineup_deadline_required");
        }
        if (*status != "open") {
            rollback(context->connection.get());
            return errorResponse(drogon::k409Conflict,
                                 "The deadline cannot change after the lineup is locked.",
                                 "lineup_locked");
        }
        auto update = execute(context->connection.get(),
            "UPDATE league_week_controls SET lineup_deadline = $4::timestamptz, updated_at = NOW() "
            "WHERE league_id = $1 AND season = $2::int AND week = $3::int AND status = 'open'",
            {leagueId, std::to_string(context->state.season), std::to_string(week), deadline});
        changed = commandOk(update) && std::string{PQcmdTuples(update.get())} == "1";
        if (changed) changed = refreshExpiredDeadlines(context->connection.get(), leagueId, context->state.season);
    } else if (action == "advance_week") {
        const auto target = std::clamp(positiveInt(body.get("currentWeek", week), week), 1, context->state.weeks + 1);
        if (target < context->state.currentWeek) {
            rollback(context->connection.get());
            return errorResponse(drogon::k409Conflict,
                                 "The active week cannot move backward.",
                                 "current_week_regression");
        }
        auto update = execute(context->connection.get(),
            "UPDATE league_schedule_states SET current_week = $2::int, updated_at = NOW() "
            "WHERE league_id = $1 AND current_week <> $2::int",
            {leagueId, std::to_string(target)});
        changed = commandOk(update) && std::string{PQcmdTuples(update.get())} == "1";
    } else {
        rollback(context->connection.get());
        return errorResponse(drogon::k400BadRequest, "Unsupported schedule action.", "unsupported_schedule_action");
    }

    if (changed) {
        auto updateVersion = execute(context->connection.get(),
            "UPDATE league_schedule_states SET version = version + 1, updated_at = NOW() "
            "WHERE league_id = $1 RETURNING version",
            {leagueId});
        if (!tuplesOk(updateVersion) || PQntuples(updateVersion.get()) == 0) {
            rollback(context->connection.get());
            return scheduleStorageUnavailable();
        }
    }
    context->state = scheduleStateRecord(context->connection.get(), leagueId);
    auto payload = scheduleStatePayload(context->connection.get(), leagueId, email,
                                        context->access, context->state);
    payload["action"] = action;
    payload["changed"] = changed;
    if (!changed && action == "lock") payload["alreadyLocked"] = true;
    if (!storeScheduleOperation(context->connection.get(), leagueId, email, key,
                                action, context->state.version, payload)
        || !commit(context->connection.get())) {
        rollback(context->connection.get());
        return scheduleStorageUnavailable();
    }
    return jsonResponse(payload);
}

drogon::HttpResponsePtr dispatchScheduleTransaction(const drogon::HttpRequestPtr &request,
                                                     const std::string &leagueId,
                                                     const std::string &email) {
    const auto body = request->getJsonObject();
    if (!body || !body->isObject()) {
        return errorResponse(drogon::k400BadRequest, "Schedule action payload is required.", "schedule_payload_required");
    }
    const auto action = lower(trim(body->get("action", "").asString()));
    if (action == "generate") return generateSchedule(request, leagueId, email, false);
    const auto week = positiveInt(body->get("week", 1), 1);
    return mutateLineupControl(request, leagueId, email, week, action, false);
}

drogon::HttpResponsePtr lineupMutationGuard(const drogon::HttpRequestPtr &request,
                                            const std::string &leagueId,
                                            const std::string &email) {
    auto context = openScheduleContext(leagueId, email, requestSeason(request));
    if (!context) return scheduleStorageUnavailable();
    if (!context->access.exists || !context->access.member) {
        rollback(context->connection.get());
        return nullptr;
    }
    const auto locked = currentLineupLocked(context->connection.get(), leagueId);
    if (!locked) {
        rollback(context->connection.get());
        return nullptr;
    }
    context->state = scheduleStateRecord(context->connection.get(), leagueId);
    auto details = scheduleConflictDetails(context->connection.get(), leagueId, email,
                                           context->access, context->state);
    rollback(context->connection.get());
    return errorResponse(drogon::k409Conflict,
                         "The current week lineup is locked.",
                         "lineup_locked",
                         false,
                         details);
}
