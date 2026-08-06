void handleDashboard(const drogon::HttpRequestPtr &request,
                     std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                     const std::optional<std::string> &jwtSecret,
                     const std::string &leagueId) {
    std::string actorEmail;
    if (!cff::http::requireAccount(request, callback, jwtSecret, actorEmail)) return;
#ifndef CFF_HAS_POSTGRES
    sendError(callback, drogon::k503ServiceUnavailable, "database_required",
              "Commissioner controls require PostgreSQL.");
#else
    actorEmail = canonicalEmail(actorEmail);
    auto connection = connectToDatabase();
    if (!connection) {
        sendError(callback, drogon::k503ServiceUnavailable, "database_unavailable",
                  "Commissioner controls are temporarily unavailable.");
        return;
    }
    const auto context = loadLeagueContext(connection.get(), leagueId, actorEmail, false);
    if (!context.exists) {
        sendError(callback, drogon::k404NotFound, "league_not_found", "League not found.");
        return;
    }
    if (!context.commissioner) {
        sendError(callback, drogon::k403Forbidden, "commissioner_required",
                  "Commissioner access is required.");
        return;
    }
    callback(jsonResponse(dashboardPayload(connection.get(), leagueId, actorEmail)));
#endif
}

void handleInvite(const drogon::HttpRequestPtr &request,
                  std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                  const std::optional<std::string> &jwtSecret,
                  const std::string &leagueId) {
    std::string actorEmail;
    if (!cff::http::requireAccount(request, callback, jwtSecret, actorEmail)) return;
#ifndef CFF_HAS_POSTGRES
    sendError(callback, drogon::k503ServiceUnavailable, "database_required",
              "Commissioner controls require PostgreSQL.");
#else
    actorEmail = canonicalEmail(actorEmail);
    const auto body = request->getJsonObject();
    const auto invitedEmail = body && body->isMember("email")
        ? canonicalEmail((*body)["email"].asString()) : "";
    if (!validEmail(invitedEmail)) {
        sendError(callback, drogon::k400BadRequest, "invalid_email",
                  "A valid manager email is required.");
        return;
    }

    auto connection = connectToDatabase();
    if (!connection || !begin(connection.get())) {
        sendError(callback, drogon::k503ServiceUnavailable, "database_unavailable",
                  "Commissioner controls are temporarily unavailable.");
        return;
    }
    const auto key = operationKey(request);
    if (const auto replay = replayedOperation(connection.get(), leagueId, key)) {
        commit(connection.get());
        callback(jsonResponse(*replay));
        return;
    }

    const auto context = loadLeagueContext(connection.get(), leagueId, actorEmail, true);
    if (!context.exists || !context.commissioner) {
        rollback(connection.get());
        sendError(callback, context.exists ? drogon::k403Forbidden : drogon::k404NotFound,
                  context.exists ? "commissioner_required" : "league_not_found",
                  context.exists ? "Commissioner access is required." : "League not found.");
        return;
    }
    if (context.draftStarted) {
        rollback(connection.get());
        sendError(callback, drogon::k409Conflict, "draft_started",
                  "Managers cannot be invited after the draft has started.");
        return;
    }

    auto existing = execute(connection.get(),
        "SELECT status FROM league_members WHERE league_id = $1 AND lower(email) = lower($2)",
        {leagueId, invitedEmail});
    const bool alreadyReserved = ok(existing.get(), PGRES_TUPLES_OK)
        && PQntuples(existing.get()) > 0 && cell(existing.get(), 0, 0) != "removed";
    if (!alreadyReserved && context.reservedManagers >= context.teamCount) {
        rollback(connection.get());
        sendError(callback, drogon::k409Conflict, "league_full",
                  "The league already has the configured number of manager spots reserved.");
        return;
    }

    auto upsert = execute(
        connection.get(),
        "INSERT INTO league_members "
        "(league_id, email, role, status, invited_by_email, created_at, updated_at) "
        "VALUES ($1, $2, 'member', 'invited', $3, NOW(), NOW()) "
        "ON CONFLICT (league_id, email) DO UPDATE SET "
        "status = CASE WHEN league_members.status = 'active' THEN 'active' ELSE 'invited' END, "
        "role = CASE WHEN league_members.status = 'active' THEN league_members.role ELSE 'member' END, "
        "invited_by_email = EXCLUDED.invited_by_email, updated_at = NOW()",
        {leagueId, invitedEmail, actorEmail});
    auto inviteList = execute(
        connection.get(),
        "UPDATE leagues SET invited_emails = "
        "ARRAY(SELECT DISTINCT unnest(invited_emails || ARRAY[$2])), updated_at = NOW() "
        "WHERE id = $1",
        {leagueId, invitedEmail});
    if (!ok(upsert.get(), PGRES_COMMAND_OK) || !ok(inviteList.get(), PGRES_COMMAND_OK)) {
        rollback(connection.get());
        sendError(callback, drogon::k500InternalServerError, "invite_failed",
                  "The invitation could not be saved.");
        return;
    }

    Json::Value metadata;
    metadata["memberEmail"] = invitedEmail;
    audit(connection.get(), leagueId, actorEmail, "Invited " + invitedEmail, metadata);
    auto payload = dashboardPayload(connection.get(), leagueId, actorEmail);
    payload["message"] = "Invitation saved.";
    if (!recordOperation(connection.get(), leagueId, key, "invite", payload)) {
        rollback(connection.get());
        sendError(callback, drogon::k500InternalServerError, "operation_record_failed",
                  "The invitation could not be recorded safely.");
        return;
    }
    if (!commit(connection.get())) {
        rollback(connection.get());
        sendError(callback, drogon::k500InternalServerError, "invite_failed",
                  "The invitation could not be committed.");
        return;
    }
    callback(jsonResponse(payload, drogon::k201Created));
#endif
}

void handleMemberAction(const drogon::HttpRequestPtr &request,
                        std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                        const std::optional<std::string> &jwtSecret,
                        const std::string &leagueId,
                        const std::string &memberEmailParameter,
                        const std::string &actionParameter) {
    std::string actorEmail;
    if (!cff::http::requireAccount(request, callback, jwtSecret, actorEmail)) return;
#ifndef CFF_HAS_POSTGRES
    sendError(callback, drogon::k503ServiceUnavailable, "database_required",
              "Commissioner controls require PostgreSQL.");
#else
    actorEmail = canonicalEmail(actorEmail);
    const auto memberEmail = canonicalEmail(memberEmailParameter);
    const auto action = lower(actionParameter);

    cff::commissioner::MemberAction memberAction;
    if (action == "approve") memberAction = cff::commissioner::MemberAction::approve;
    else if (action == "reject") memberAction = cff::commissioner::MemberAction::reject;
    else if (action == "remove") memberAction = cff::commissioner::MemberAction::remove;
    else if (action == "promote") memberAction = cff::commissioner::MemberAction::promote;
    else if (action == "demote") memberAction = cff::commissioner::MemberAction::demote;
    else if (action == "transfer") memberAction = cff::commissioner::MemberAction::transfer;
    else {
        sendError(callback, drogon::k400BadRequest, "unsupported_action",
                  "Unsupported commissioner action.");
        return;
    }

    auto connection = connectToDatabase();
    if (!connection || !begin(connection.get())) {
        sendError(callback, drogon::k503ServiceUnavailable, "database_unavailable",
                  "Commissioner controls are temporarily unavailable.");
        return;
    }

    const auto key = operationKey(request);
    if (const auto replay = replayedOperation(connection.get(), leagueId, key)) {
        commit(connection.get());
        callback(jsonResponse(*replay));
        return;
    }

    const auto context = loadLeagueContext(connection.get(), leagueId, actorEmail, true);
    if (!context.exists || !context.commissioner) {
        rollback(connection.get());
        sendError(callback, context.exists ? drogon::k403Forbidden : drogon::k404NotFound,
                  context.exists ? "commissioner_required" : "league_not_found",
                  context.exists ? "Commissioner access is required." : "League not found.");
        return;
    }

    const auto member = loadMemberState(connection.get(), leagueId, memberEmail);
    const cff::commissioner::LeagueState league{
        context.teamCount, context.activeManagers, context.draftStarted};
    const auto decision = cff::commissioner::evaluateMemberAction(
        memberAction, league, member, context.owner);
    if (!decision.allowed) {
        rollback(connection.get());
        sendError(callback, statusForDecision(decision), decision.code,
                  decision.message, decision.blockers);
        return;
    }

    bool changed = false;
    std::string summary;
    Json::Value metadata;
    metadata["memberEmail"] = memberEmail;
    metadata["action"] = action;

    if (action == "approve") {
        auto update = execute(
            connection.get(),
            "UPDATE league_members SET status = 'active', joined_at = COALESCE(joined_at, NOW()), "
            "updated_at = NOW() WHERE league_id = $1 AND lower(email) = lower($2) "
            "AND status IN ('invited', 'pending')",
            {leagueId, memberEmail});
        changed = ok(update.get(), PGRES_COMMAND_OK) && std::string{PQcmdTuples(update.get())} == "1";
        execute(connection.get(),
            "INSERT INTO waiver_priorities (league_id, manager_email, priority) "
            "VALUES ($1, $2, COALESCE((SELECT MAX(priority) + 1 FROM waiver_priorities WHERE league_id = $1), 1)) "
            "ON CONFLICT (league_id, manager_email) DO NOTHING",
            {leagueId, memberEmail});
        execute(connection.get(),
            "UPDATE draft_states SET draft_order = CASE "
            "WHEN NOT ($2 = ANY(draft_order)) THEN array_append(draft_order, $2) ELSE draft_order END, "
            "updated_at = NOW() WHERE league_id = $1 AND status = 'not_started'",
            {leagueId, memberEmail});
        summary = "Approved " + memberEmail;
    } else if (action == "reject") {
        auto update = execute(
            connection.get(),
            "UPDATE league_members SET status = 'removed', role = 'member', updated_at = NOW() "
            "WHERE league_id = $1 AND lower(email) = lower($2) AND status IN ('invited', 'pending')",
            {leagueId, memberEmail});
        changed = ok(update.get(), PGRES_COMMAND_OK) && std::string{PQcmdTuples(update.get())} == "1";
        execute(connection.get(),
            "UPDATE leagues SET invited_emails = array_remove(invited_emails, $2), updated_at = NOW() "
            "WHERE id = $1", {leagueId, memberEmail});
        summary = "Rejected " + memberEmail;
    } else if (action == "remove") {
        execute(connection.get(),
            "UPDATE waiver_claims SET status = 'cancelled', processed_at = NOW(), updated_at = NOW(), "
            "resolved_by_email = $3 WHERE league_id = $1 AND lower(manager_email) = lower($2) "
            "AND status = 'pending'", {leagueId, memberEmail, actorEmail});
        execute(connection.get(),
            "UPDATE trade_offers SET status = 'cancelled', resolved_at = NOW() "
            "WHERE league_id = $1 AND status IN ('pending', 'accepted') "
            "AND (lower(offered_by_email) = lower($2) OR lower(offered_to_email) = lower($2))",
            {leagueId, memberEmail});
        execute(connection.get(),
            "DELETE FROM draft_queues WHERE league_id = $1 AND lower(manager_email) = lower($2)",
            {leagueId, memberEmail});
        execute(connection.get(),
            "DELETE FROM draft_readiness WHERE league_id = $1 AND lower(manager_email) = lower($2)",
            {leagueId, memberEmail});
        execute(connection.get(),
            "DELETE FROM waiver_priorities WHERE league_id = $1 AND lower(manager_email) = lower($2)",
            {leagueId, memberEmail});
        execute(connection.get(),
            "UPDATE draft_states SET draft_order = array_remove(draft_order, $2), updated_at = NOW() "
            "WHERE league_id = $1 AND status = 'not_started'",
            {leagueId, memberEmail});
        auto update = execute(
            connection.get(),
            "UPDATE league_members SET status = 'removed', role = 'member', updated_at = NOW() "
            "WHERE league_id = $1 AND lower(email) = lower($2)",
            {leagueId, memberEmail});
        execute(connection.get(),
            "UPDATE leagues SET invited_emails = array_remove(invited_emails, $2), updated_at = NOW() "
            "WHERE id = $1", {leagueId, memberEmail});
        changed = ok(update.get(), PGRES_COMMAND_OK) && std::string{PQcmdTuples(update.get())} == "1";
        summary = "Removed " + memberEmail;
    } else if (action == "promote" || action == "demote") {
        const auto role = action == "promote" ? "commissioner" : "member";
        auto update = execute(
            connection.get(),
            "UPDATE league_members SET role = $3, updated_at = NOW() "
            "WHERE league_id = $1 AND lower(email) = lower($2) AND status = 'active'",
            {leagueId, memberEmail, role});
        changed = ok(update.get(), PGRES_COMMAND_OK) && std::string{PQcmdTuples(update.get())} == "1";
        summary = std::string(action == "promote" ? "Granted commissioner access to " : "Removed commissioner access from ")
            + memberEmail;
    } else if (action == "transfer") {
        auto ownedCount = execute(
            connection.get(),
            "SELECT COUNT(*) FROM leagues WHERE lower(account_email) = lower($1) AND id <> $2",
            {memberEmail, leagueId});
        if (!ok(ownedCount.get(), PGRES_TUPLES_OK)
            || cellInt(ownedCount.get(), 0, 0) >= 3) {
            rollback(connection.get());
            sendError(callback, drogon::k409Conflict, "recipient_league_limit",
                      "The selected manager already owns the maximum number of leagues.");
            return;
        }
        auto updateLeague = execute(
            connection.get(),
            "UPDATE leagues SET account_email = $2, updated_at = NOW() "
            "WHERE id = $1 AND lower(account_email) = lower($3)",
            {leagueId, memberEmail, actorEmail});
        auto oldOwner = execute(
            connection.get(),
            "UPDATE league_members SET role = 'member', updated_at = NOW() "
            "WHERE league_id = $1 AND lower(email) = lower($2)",
            {leagueId, actorEmail});
        auto newOwner = execute(
            connection.get(),
            "UPDATE league_members SET role = 'commissioner', status = 'active', updated_at = NOW() "
            "WHERE league_id = $1 AND lower(email) = lower($2)",
            {leagueId, memberEmail});
        changed = ok(updateLeague.get(), PGRES_COMMAND_OK)
            && std::string{PQcmdTuples(updateLeague.get())} == "1"
            && ok(oldOwner.get(), PGRES_COMMAND_OK)
            && ok(newOwner.get(), PGRES_COMMAND_OK);
        summary = "Transferred league ownership to " + memberEmail;
    }

    if (!changed) {
        rollback(connection.get());
        sendError(callback, drogon::k409Conflict, "action_not_applied",
                  "The commissioner action could not be applied to the current league state.");
        return;
    }

    audit(connection.get(), leagueId, actorEmail, summary, metadata);
    auto payload = dashboardPayload(connection.get(), leagueId, actorEmail);
    payload["message"] = summary;
    payload["action"] = action;
    if (!recordOperation(connection.get(), leagueId, key, action, payload)) {
        rollback(connection.get());
        sendError(callback, drogon::k500InternalServerError, "operation_record_failed",
                  "The commissioner action could not be recorded safely.");
        return;
    }
    if (!commit(connection.get())) {
        rollback(connection.get());
        sendError(callback, drogon::k500InternalServerError, "commit_failed",
                  "The commissioner action could not be committed.");
        return;
    }
    callback(jsonResponse(payload));
#endif
}
