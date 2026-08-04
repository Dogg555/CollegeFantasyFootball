struct ScheduleContext {
    PgConnection connection;
    ScheduleAccess access;
    ScheduleRecord schedule;
};

std::optional<ScheduleContext> openScheduleContext(const std::string &leagueId,
                                                   const std::string &email,
                                                   int season) {
    auto connection = connectDb();
    if (!connection || !begin(connection.get())
        || !lockScheduleLeague(connection.get(), leagueId, season)) {
        if (connection) rollback(connection.get());
        return std::nullopt;
    }
    auto access = scheduleAccess(connection.get(), leagueId, email);
    if (access.exists && !ensureScheduleState(connection.get(), leagueId, season)) {
        rollback(connection.get());
        return std::nullopt;
    }
    ScheduleContext context;
    context.connection = std::move(connection);
    context.access = access;
    context.schedule = scheduleRecord(context.connection.get(), leagueId, season);
    return context;
}

long long advanceScheduleVersion(PGconn *connection,
                                 const std::string &leagueId,
                                 int season) {
    auto result = execute(connection,
        "UPDATE schedule_states SET version = version + 1, updated_at = NOW() "
        "WHERE league_id = $1 AND season = $2::int RETURNING version",
        {leagueId, std::to_string(season)});
    if (!tuplesOk(result) || PQntuples(result.get()) == 0) return -1;
    return cellInt64(result.get(), 0, 0, -1);
}

bool validDeadline(const std::string &deadline) {
    if (deadline.empty()) return true;
    return deadline.size() >= 20
        && deadline.find('T') != std::string::npos
        && (deadline.back() == 'Z' || deadline.find('+') != std::string::npos);
}

std::vector<std::string> activeManagerEmails(const Json::Value &members) {
    std::vector<std::string> managers;
    for (const auto &member : members) {
        const auto email = canonicalEmail(member.get("email", "").asString());
        if (!email.empty()) managers.push_back(email);
    }
    std::sort(managers.begin(), managers.end());
    managers.erase(std::unique(managers.begin(), managers.end()), managers.end());
    return managers;
}

struct LineupCandidate {
    std::string manager;
    Json::Value snapshot{Json::arrayValue};
    Json::Value errors{Json::arrayValue};
    std::string existingStatus{"open"};
    std::string existingReason;
};

std::unordered_map<std::string, Json::Value> existingLineupsByManager(
    PGconn *connection,
    const std::string &leagueId,
    int season,
    int week) {
    std::unordered_map<std::string, Json::Value> result;
    for (const auto &lineup : lineupStatesPayload(connection, leagueId, season, week)) {
        result[canonicalEmail(lineup.get("managerEmail", "").asString())] = lineup;
    }
    return result;
}

bool updateWeekLockStatus(PGconn *connection,
                          const std::string &leagueId,
                          int season,
                          int week) {
    const auto status = allManagersLocked(connection, leagueId, season, week)
        ? "locked"
        : "open";
    return commandOk(execute(connection,
        "UPDATE schedule_week_states SET status = $4, version = version + 1, updated_at = NOW() "
        "WHERE league_id = $1 AND season = $2::int AND week = $3::int",
        {leagueId, std::to_string(season), std::to_string(week), status}));
}

bool lockManagers(PGconn *connection,
                  const std::string &leagueId,
                  int season,
                  int week,
                  const std::vector<std::string> &managers,
                  const Json::Value &rosterRules,
                  const std::string &reason,
                  bool requireValid,
                  Json::Value &validationErrors,
                  bool &changed) {
    validationErrors = Json::Value{Json::arrayValue};
    changed = false;
    if (!ensureWeekState(connection, leagueId, season, week)
        || !ensureLineupStates(connection, leagueId, season, week)) return false;

    const auto existing = existingLineupsByManager(connection, leagueId, season, week);
    std::vector<LineupCandidate> candidates;
    for (const auto &manager : managers) {
        LineupCandidate candidate;
        candidate.manager = canonicalEmail(manager);
        const auto found = existing.find(candidate.manager);
        if (found != existing.end()) {
            candidate.existingStatus = lower(found->second.get("status", "open").asString());
            candidate.existingReason = lower(found->second.get("lockReason", "").asString());
            if (candidate.existingStatus == "locked" || candidate.existingStatus == "finalized") {
                candidate.snapshot = found->second.get("lineup", Json::Value{Json::arrayValue});
            }
        }
        if (!candidate.snapshot.isArray() || candidate.snapshot.empty()) {
            candidate.snapshot = currentLineupSnapshot(connection, leagueId, candidate.manager);
        }
        candidate.errors = lineupErrors(candidate.manager, candidate.snapshot, rosterRules);
        if (candidate.errors.size() > 0) {
            for (const auto &error : candidate.errors) validationErrors.append(error);
        }
        candidates.push_back(candidate);
    }

    if (requireValid && validationErrors.size() > 0) return true;

    for (const auto &candidate : candidates) {
        const bool same = candidate.existingStatus == "locked"
            && candidate.existingReason == reason;
        if (same) continue;
        auto update = execute(connection,
            "UPDATE lineup_week_states SET version = version + 1, status = 'locked', "
            "lock_reason = $5, lineup_snapshot = $6::jsonb, validation_errors = $7::jsonb, "
            "locked_at = COALESCE(locked_at, NOW()), unlocked_at = NULL, updated_at = NOW() "
            "WHERE league_id = $1 AND season = $2::int AND week = $3::int "
            "AND lower(manager_email) = lower($4)",
            {leagueId, std::to_string(season), std::to_string(week), candidate.manager,
             reason, jsonToString(candidate.snapshot), jsonToString(candidate.errors)});
        if (!commandOk(update) || std::string{PQcmdTuples(update.get())} != "1") return false;
        changed = true;
    }

    if (changed && !updateWeekLockStatus(connection, leagueId, season, week)) return false;
    return true;
}

bool unlockManagers(PGconn *connection,
                    const std::string &leagueId,
                    int season,
                    int week,
                    const std::vector<std::string> &managers,
                    bool &changed,
                    std::string &blockedReason) {
    changed = false;
    blockedReason.clear();
    const auto weekState = weekStateRecord(connection, leagueId, season, week);
    if (cff::schedule_lineup_lifecycle::deadlinePassed(weekState.deadline, isoNow())) {
        blockedReason = "lineup_deadline_passed";
        return true;
    }
    if (scoringWeekStatus(connection, leagueId, season, week) != "unscored") {
        blockedReason = "week_scoring_started";
        return true;
    }

    const auto existing = existingLineupsByManager(connection, leagueId, season, week);
    for (const auto &manager : managers) {
        const auto email = canonicalEmail(manager);
        const auto found = existing.find(email);
        if (found == existing.end()) continue;
        const auto status = lower(found->second.get("status", "open").asString());
        const auto reason = lower(found->second.get("lockReason", "").asString());
        if (status == "finalized") {
            blockedReason = "week_finalized";
            return true;
        }
        if (reason == "scoring") {
            blockedReason = "scoring_lock_permanent";
            return true;
        }
    }

    for (const auto &manager : managers) {
        auto update = execute(connection,
            "UPDATE lineup_week_states SET version = version + 1, status = 'open', "
            "lock_reason = '', lineup_snapshot = '[]'::jsonb, validation_errors = '[]'::jsonb, "
            "locked_at = NULL, unlocked_at = NOW(), updated_at = NOW() "
            "WHERE league_id = $1 AND season = $2::int AND week = $3::int "
            "AND lower(manager_email) = lower($4) AND status = 'locked'",
            {leagueId, std::to_string(season), std::to_string(week), canonicalEmail(manager)});
        if (!commandOk(update)) return false;
        if (std::string{PQcmdTuples(update.get())} == "1") changed = true;
    }
    if (changed && !updateWeekLockStatus(connection, leagueId, season, week)) return false;
    return true;
}

bool autoLockExpired(PGconn *connection,
                     const std::string &leagueId,
                     int season,
                     int week,
                     const Json::Value &rosterRules,
                     bool &changed) {
    changed = false;
    const auto weekState = weekStateRecord(connection, leagueId, season, week);
    if (!cff::schedule_lineup_lifecycle::deadlinePassed(weekState.deadline, isoNow())
        || scoringWeekStatus(connection, leagueId, season, week) == "final") return true;
    const auto members = activeMembersPayload(connection, leagueId);
    Json::Value ignoredErrors(Json::arrayValue);
    return lockManagers(connection, leagueId, season, week,
                        activeManagerEmails(members), rosterRules,
                        "deadline", false, ignoredErrors, changed);
}

drogon::HttpResponsePtr getScheduleState(const std::string &leagueId,
                                         const std::string &email,
                                         int season,
                                         int week) {
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
    if (!ensureWeekState(context->connection.get(), leagueId, season, week)
        || !ensureLineupStates(context->connection.get(), leagueId, season, week)) {
        rollback(context->connection.get());
        return scheduleStorageUnavailable();
    }
    bool autoLocked = false;
    if (!autoLockExpired(context->connection.get(), leagueId, season, week,
                         context->access.rosterRules, autoLocked)) {
        rollback(context->connection.get());
        return scheduleStorageUnavailable();
    }
    if (autoLocked && advanceScheduleVersion(context->connection.get(), leagueId, season) < 0) {
        rollback(context->connection.get());
        return scheduleStorageUnavailable();
    }
    context->schedule = scheduleRecord(context->connection.get(), leagueId, season);
    auto payload = scheduleStatePayload(context->connection.get(), leagueId, email,
                                        context->access, season, week, context->schedule);
    payload["deadlineAutoLocked"] = autoLocked;
    if (!commit(context->connection.get())) {
        rollback(context->connection.get());
        return scheduleStorageUnavailable();
    }
    return jsonResponse(payload);
}

drogon::HttpResponsePtr generateSchedule(const drogon::HttpRequestPtr &request,
                                         const std::string &leagueId,
                                         const std::string &email,
                                         int season,
                                         int weeks,
                                         bool legacy) {
    const auto body = request->getJsonObject();
    const Json::Value requestBody = body && body->isObject() ? *body : Json::Value{Json::objectValue};
    const auto key = operationKey(request);
    if (!legacy && key.empty()) {
        return errorResponse(drogon::k400BadRequest,
                             "An Idempotency-Key is required for schedule mutations.",
                             "idempotency_key_required");
    }
    weeks = std::clamp(weeks, 1, 15);
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
    if (const auto replay = operationReplay(context->connection.get(), leagueId, email, key, "generate")) {
        if (!(*replay)["operationTypeMatches"].asBool()) {
            rollback(context->connection.get());
            return errorResponse(drogon::k409Conflict,
                                 "This idempotency key was already used for another schedule action.",
                                 "idempotency_key_conflict");
        }
        auto payload = *replay;
        payload.removeMember("operationTypeMatches");
        if (!commit(context->connection.get())) {
            rollback(context->connection.get());
            return scheduleStorageUnavailable();
        }
        return jsonResponse(legacy ? payload["schedule"] : payload);
    }
    if (!cff::schedule_lineup_lifecycle::expectedVersionMatches(
            context->schedule.version, requestBody, !legacy)) {
        const auto details = scheduleConflictDetails(context->connection.get(), leagueId, email,
            context->access, season, requestWeek(request));
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict,
                             "Schedule state changed. Load the latest season before retrying.",
                             "schedule_state_conflict",
                             false,
                             details);
    }

    const auto members = activeMembersPayload(context->connection.get(), leagueId);
    const auto managerOrder = cff::schedule_lineup_lifecycle::canonicalManagerOrder(members);
    if (managerOrder.size() < 2) {
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict,
                             "At least two active managers are required.",
                             "insufficient_active_managers");
    }
    const auto inputHash = cff::schedule_lineup_lifecycle::scheduleInputHash(
        managerOrder, season, weeks);
    if (context->schedule.inputHash == inputHash
        && context->schedule.weeks == weeks
        && context->schedule.schedule.isArray()
        && !context->schedule.schedule.empty()) {
        auto payload = scheduleStatePayload(context->connection.get(), leagueId, email,
            context->access, season, requestWeek(request), context->schedule);
        payload["unchanged"] = true;
        if (!storeOperation(context->connection.get(), leagueId, season, email, key,
                            "generate", context->schedule.version, payload)
            || !commit(context->connection.get())) {
            rollback(context->connection.get());
            return scheduleStorageUnavailable();
        }
        return jsonResponse(legacy ? payload["schedule"] : payload);
    }
    if (scheduleProtected(context->connection.get(), leagueId, season)) {
        const auto details = scheduleConflictDetails(context->connection.get(), leagueId, email,
            context->access, season, requestWeek(request));
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict,
                             "The schedule cannot change after a lineup is locked or scoring begins.",
                             "schedule_locked",
                             false,
                             details);
    }

    const auto nextVersion = context->schedule.version + 1;
    const auto schedule = cff::schedule_lineup_lifecycle::buildDeterministicSchedule(
        managerOrder, leagueId, season, weeks);
    if (!persistSchedule(context->connection.get(), leagueId, season, nextVersion,
                         inputHash, weeks, managerOrder, schedule)) {
        rollback(context->connection.get());
        return scheduleStorageUnavailable();
    }
    context->schedule = scheduleRecord(context->connection.get(), leagueId, season);
    auto payload = scheduleStatePayload(context->connection.get(), leagueId, email,
        context->access, season, requestWeek(request), context->schedule);
    payload["generated"] = true;
    Json::Value metadata(Json::objectValue);
    metadata["season"] = season;
    metadata["weeks"] = weeks;
    metadata["scheduleVersion"] = Json::Int64(nextVersion);
    if (!addScheduleTransaction(context->connection.get(), leagueId, email,
                                "Schedule", "Generated deterministic season schedule", metadata)
        || !storeOperation(context->connection.get(), leagueId, season, email, key,
                           "generate", nextVersion, payload)
        || !commit(context->connection.get())) {
        rollback(context->connection.get());
        return scheduleStorageUnavailable();
    }
    return jsonResponse(legacy ? payload["schedule"] : payload);
}

drogon::HttpResponsePtr setLineupDeadline(const drogon::HttpRequestPtr &request,
                                          const std::string &leagueId,
                                          const std::string &email,
                                          int season,
                                          int week) {
    const auto body = request->getJsonObject();
    const Json::Value requestBody = body && body->isObject() ? *body : Json::Value{Json::objectValue};
    const auto key = operationKey(request);
    const auto deadline = trim(requestBody.get("lineupDeadline", requestBody.get("deadline", "")).asString());
    if (key.empty()) {
        return errorResponse(drogon::k400BadRequest,
                             "An Idempotency-Key is required for deadline changes.",
                             "idempotency_key_required");
    }
    if (!validDeadline(deadline)) {
        return errorResponse(drogon::k400BadRequest,
                             "lineupDeadline must be an ISO-8601 timestamp or empty.",
                             "invalid_lineup_deadline");
    }
    auto context = openScheduleContext(leagueId, email, season);
    if (!context) return scheduleStorageUnavailable();
    if (!context->access.commissioner) {
        rollback(context->connection.get());
        return errorResponse(drogon::k403Forbidden,
                             "Only the league commissioner can change lineup deadlines.",
                             "commissioner_required");
    }
    if (const auto replay = operationReplay(context->connection.get(), leagueId, email, key, "set_deadline")) {
        if (!(*replay)["operationTypeMatches"].asBool()) {
            rollback(context->connection.get());
            return errorResponse(drogon::k409Conflict,
                                 "This idempotency key was already used for another schedule action.",
                                 "idempotency_key_conflict");
        }
        auto payload = *replay;
        payload.removeMember("operationTypeMatches");
        if (!commit(context->connection.get())) {
            rollback(context->connection.get());
            return scheduleStorageUnavailable();
        }
        return jsonResponse(payload);
    }
    if (!cff::schedule_lineup_lifecycle::expectedVersionMatches(
            context->schedule.version, requestBody, true)) {
        const auto details = scheduleConflictDetails(context->connection.get(), leagueId, email,
            context->access, season, week);
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict,
                             "Schedule state changed. Load the latest week before retrying.",
                             "schedule_state_conflict",
                             false,
                             details);
    }
    if (!ensureWeekState(context->connection.get(), leagueId, season, week)
        || !ensureLineupStates(context->connection.get(), leagueId, season, week)) {
        rollback(context->connection.get());
        return scheduleStorageUnavailable();
    }
    const auto currentWeek = weekStateRecord(context->connection.get(), leagueId, season, week);
    if (lower(currentWeek.status) != "open"
        || scoringWeekStatus(context->connection.get(), leagueId, season, week) != "unscored") {
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict,
                             "The deadline cannot change after a lineup is locked or scoring begins.",
                             "lineup_deadline_locked");
    }
    auto update = execute(context->connection.get(),
        "UPDATE schedule_week_states SET lineup_deadline = NULLIF($4, '')::timestamptz, "
        "version = version + 1, updated_at = NOW() "
        "WHERE league_id = $1 AND season = $2::int AND week = $3::int",
        {leagueId, std::to_string(season), std::to_string(week), deadline});
    const auto version = advanceScheduleVersion(context->connection.get(), leagueId, season);
    if (!commandOk(update) || version < 0) {
        rollback(context->connection.get());
        return scheduleStorageUnavailable();
    }
    context->schedule = scheduleRecord(context->connection.get(), leagueId, season);
    auto payload = scheduleStatePayload(context->connection.get(), leagueId, email,
        context->access, season, week, context->schedule);
    payload["deadlineUpdated"] = true;
    if (!storeOperation(context->connection.get(), leagueId, season, email, key,
                        "set_deadline", version, payload)
        || !commit(context->connection.get())) {
        rollback(context->connection.get());
        return scheduleStorageUnavailable();
    }
    return jsonResponse(payload);
}

drogon::HttpResponsePtr mutateLineupLock(const drogon::HttpRequestPtr &request,
                                         const std::string &leagueId,
                                         const std::string &email,
                                         int season,
                                         int week,
                                         bool unlock,
                                         bool legacy = false) {
    const auto body = request->getJsonObject();
    const Json::Value requestBody = body && body->isObject() ? *body : Json::Value{Json::objectValue};
    const auto key = operationKey(request);
    const auto operationType = unlock ? "unlock" : "lock";
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
    if (!context->access.member) {
        rollback(context->connection.get());
        return errorResponse(drogon::k403Forbidden,
                             "Active league membership is required.",
                             "league_membership_required");
    }
    if (const auto replay = operationReplay(context->connection.get(), leagueId, email, key, operationType)) {
        if (!(*replay)["operationTypeMatches"].asBool()) {
            rollback(context->connection.get());
            return errorResponse(drogon::k409Conflict,
                                 "This idempotency key was already used for another lineup action.",
                                 "idempotency_key_conflict");
        }
        auto payload = *replay;
        payload.removeMember("operationTypeMatches");
        if (!commit(context->connection.get())) {
            rollback(context->connection.get());
            return scheduleStorageUnavailable();
        }
        return jsonResponse(payload);
    }
    if (!cff::schedule_lineup_lifecycle::expectedVersionMatches(
            context->schedule.version, requestBody, !legacy)) {
        const auto details = scheduleConflictDetails(context->connection.get(), leagueId, email,
            context->access, season, week);
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict,
                             "Schedule state changed. Load the latest lineup before retrying.",
                             "schedule_state_conflict",
                             false,
                             details);
    }
    if (!ensureWeekState(context->connection.get(), leagueId, season, week)
        || !ensureLineupStates(context->connection.get(), leagueId, season, week)) {
        rollback(context->connection.get());
        return scheduleStorageUnavailable();
    }

    const bool all = requestBody.get("all", false).asBool();
    auto target = canonicalEmail(requestBody.get("managerEmail", email).asString());
    if (all && !context->access.commissioner) {
        rollback(context->connection.get());
        return errorResponse(drogon::k403Forbidden,
                             "Only the commissioner can lock or unlock every lineup.",
                             "commissioner_required");
    }
    if (target != email && !context->access.commissioner) {
        rollback(context->connection.get());
        return errorResponse(drogon::k403Forbidden,
                             "Managers may only change their own lineup lock.",
                             "lineup_actor_mismatch");
    }
    const auto active = activeManagerEmails(activeMembersPayload(context->connection.get(), leagueId));
    std::vector<std::string> managers = all ? active : std::vector<std::string>{target};
    if (std::find(active.begin(), active.end(), target) == active.end() && !all) {
        rollback(context->connection.get());
        return errorResponse(drogon::k404NotFound,
                             "The target manager is not active in this league.",
                             "manager_not_found");
    }

    bool changed = false;
    if (unlock) {
        std::string blockedReason;
        if (!unlockManagers(context->connection.get(), leagueId, season, week,
                            managers, changed, blockedReason)) {
            rollback(context->connection.get());
            return scheduleStorageUnavailable();
        }
        if (!blockedReason.empty()) {
            rollback(context->connection.get());
            return errorResponse(drogon::k409Conflict,
                                 "This lineup can no longer be unlocked.",
                                 blockedReason);
        }
    } else {
        Json::Value errors(Json::arrayValue);
        if (!lockManagers(context->connection.get(), leagueId, season, week,
                          managers, context->access.rosterRules,
                          "manual", true, errors, changed)) {
            rollback(context->connection.get());
            return scheduleStorageUnavailable();
        }
        if (errors.size() > 0) {
            Json::Value details(Json::objectValue);
            details["lineupErrors"] = errors;
            rollback(context->connection.get());
            return errorResponse(drogon::k409Conflict,
                                 "The lineup is incomplete or invalid.",
                                 "lineup_invalid",
                                 false,
                                 details);
        }
    }

    long long version = context->schedule.version;
    if (changed) {
        version = advanceScheduleVersion(context->connection.get(), leagueId, season);
        if (version < 0) {
            rollback(context->connection.get());
            return scheduleStorageUnavailable();
        }
    }
    context->schedule = scheduleRecord(context->connection.get(), leagueId, season);
    auto payload = scheduleStatePayload(context->connection.get(), leagueId, email,
        context->access, season, week, context->schedule);
    payload[unlock ? "unlocked" : "locked"] = changed;
    payload["unchanged"] = !changed;
    Json::Value metadata(Json::objectValue);
    metadata["season"] = season;
    metadata["week"] = week;
    metadata["all"] = all;
    if (changed && !addScheduleTransaction(context->connection.get(), leagueId, email,
            unlock ? "Lineup Unlocked" : "Lineup Locked",
            (unlock ? "Unlocked" : "Locked") + std::string{" week "} + std::to_string(week) + " lineup",
            metadata)) {
        rollback(context->connection.get());
        return scheduleStorageUnavailable();
    }
    if (!storeOperation(context->connection.get(), leagueId, season, email, key,
                        operationType, version, payload)
        || !commit(context->connection.get())) {
        rollback(context->connection.get());
        return scheduleStorageUnavailable();
    }
    return jsonResponse(payload);
}

drogon::HttpResponsePtr prepareLineupsForScoringInternal(const std::string &leagueId,
                                                         const std::string &email,
                                                         int season,
                                                         int week) {
    auto context = openScheduleContext(leagueId, email, season);
    if (!context) return scheduleStorageUnavailable();
    if (!context->access.exists) {
        rollback(context->connection.get());
        return errorResponse(drogon::k404NotFound, "League not found.", "league_not_found");
    }
    if (!context->access.commissioner) {
        rollback(context->connection.get());
        return errorResponse(drogon::k403Forbidden,
                             "Only the commissioner can begin scoring.",
                             "commissioner_required");
    }
    const auto scoringStatus = scoringWeekStatus(context->connection.get(), leagueId, season, week);
    if (scoringStatus == "final") {
        commit(context->connection.get());
        return nullptr;
    }
    const auto managers = activeManagerEmails(activeMembersPayload(context->connection.get(), leagueId));
    Json::Value errors(Json::arrayValue);
    bool changed = false;
    if (!lockManagers(context->connection.get(), leagueId, season, week,
                      managers, context->access.rosterRules,
                      "scoring", true, errors, changed)) {
        rollback(context->connection.get());
        return scheduleStorageUnavailable();
    }
    if (errors.size() > 0) {
        Json::Value details(Json::objectValue);
        details["lineupErrors"] = errors;
        rollback(context->connection.get());
        return errorResponse(drogon::k409Conflict,
                             "Every active manager must have a valid lineup before scoring.",
                             "lineup_invalid",
                             false,
                             details);
    }
    if (changed && advanceScheduleVersion(context->connection.get(), leagueId, season) < 0) {
        rollback(context->connection.get());
        return scheduleStorageUnavailable();
    }
    if (!commit(context->connection.get())) {
        rollback(context->connection.get());
        return scheduleStorageUnavailable();
    }
    return nullptr;
}

drogon::HttpResponsePtr dispatchScheduleTransaction(const drogon::HttpRequestPtr &request,
                                                     const std::string &leagueId,
                                                     const std::string &email) {
    const auto body = request->getJsonObject();
    if (!body || !body->isObject()) {
        return errorResponse(drogon::k400BadRequest,
                             "A schedule transaction payload is required.",
                             "schedule_payload_required");
    }
    const auto action = lower(body->get("action", "").asString());
    const auto season = requestSeason(request);
    const auto week = requestWeek(request);
    if (action == "generate") {
        return generateSchedule(request, leagueId, email, season,
                                positiveInt(body->get("weeks", 12), 12), false);
    }
    if (action == "set_deadline") {
        return setLineupDeadline(request, leagueId, email, season, week);
    }
    if (action == "lock") {
        return mutateLineupLock(request, leagueId, email, season, week, false);
    }
    if (action == "unlock") {
        return mutateLineupLock(request, leagueId, email, season, week, true);
    }
    return errorResponse(drogon::k400BadRequest,
                         "Unknown schedule transaction action.",
                         "unknown_schedule_action");
}

drogon::HttpResponsePtr rosterSlotGuard(const std::string &leagueId,
                                        const std::string &email) {
    auto connection = connectDb();
    if (!connection) return scheduleStorageUnavailable();
    if (!managerHasActiveLineupLock(connection.get(), leagueId, email)) return nullptr;
    return errorResponse(drogon::k409Conflict,
                         "The active weekly lineup is locked.",
                         "lineup_locked");
}

drogon::HttpResponsePtr rosterDropGuard(const drogon::HttpRequestPtr &request,
                                        const std::string &leagueId,
                                        const std::string &email) {
    const auto body = request->getJsonObject();
    const auto playerId = body && body->isObject() ? body->get("playerId", "").asString() : "";
    if (playerId.empty()) return nullptr;
    auto connection = connectDb();
    if (!connection) return scheduleStorageUnavailable();
    if (!playerIsLockedStarter(connection.get(), leagueId, email, playerId)) return nullptr;
    return errorResponse(drogon::k409Conflict,
                         "This starter is protected by a locked weekly lineup.",
                         "lineup_locked");
}
