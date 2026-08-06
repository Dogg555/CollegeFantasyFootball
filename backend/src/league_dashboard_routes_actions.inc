Json::Value loadWaivers(
    PGconn *connection,
    const std::string &leagueId,
    const std::string &actorEmail,
    Json::Value &freshness,
    int &pendingCount) {
    auto result = execute(
        connection,
        "SELECT id, add_player_id, COALESCE(add_player_snapshot->>'name', add_player_id), "
        "COALESCE(drop_player_id, ''), priority, "
        "to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') "
        "FROM waiver_claims WHERE league_id = $1 AND lower(manager_email) = lower($2) "
        "AND status = 'pending' ORDER BY claim_order, created_at LIMIT 5",
        {leagueId, actorEmail});
    Json::Value payload(Json::objectValue);
    Json::Value items(Json::arrayValue);
    if (!tuplesOk(result.get())) {
        markUnavailable(freshness, "waivers");
        payload["available"] = false;
        payload["pendingCount"] = 0;
        payload["items"] = items;
        return payload;
    }
    for (int row = 0; row < PQntuples(result.get()); ++row) {
        Json::Value item(Json::objectValue);
        item["id"] = cell(result.get(), row, 0);
        item["addPlayerId"] = cell(result.get(), row, 1);
        item["addPlayerName"] = cell(result.get(), row, 2);
        item["dropPlayerId"] = cell(result.get(), row, 3);
        item["priority"] = cellInt(result.get(), row, 4);
        item["createdAt"] = cell(result.get(), row, 5);
        items.append(item);
    }
    auto count = execute(
        connection,
        "SELECT COUNT(*) FROM waiver_claims WHERE league_id = $1 "
        "AND lower(manager_email) = lower($2) AND status = 'pending'",
        {leagueId, actorEmail});
    pendingCount = tuplesOk(count.get()) && PQntuples(count.get()) > 0
        ? cellInt(count.get(), 0, 0)
        : static_cast<int>(items.size());
    payload["available"] = true;
    payload["pendingCount"] = pendingCount;
    payload["items"] = items;
    return payload;
}

Json::Value loadTrades(
    PGconn *connection,
    const std::string &leagueId,
    const std::string &actorEmail,
    bool commissioner,
    Json::Value &freshness,
    int &actionRequiredCount) {
    const std::vector<std::string> parameters{
        leagueId, actorEmail, commissioner ? "true" : "false"
    };
    auto counts = execute(
        connection,
        "SELECT COUNT(*), COUNT(*) FILTER (WHERE "
        " (status = 'pending' AND lower(offered_to_email) = lower($2)) "
        " OR ($3 = 'true' AND status = 'accepted' AND requires_approval)) "
        "FROM trade_offers WHERE league_id = $1 AND status IN ('pending', 'accepted') "
        "AND (lower(offered_by_email) = lower($2) OR lower(offered_to_email) = lower($2) "
        " OR ($3 = 'true' AND status = 'accepted' AND requires_approval))",
        parameters);

    int openCount = 0;
    bool countsAvailable = tuplesOk(counts.get()) && PQntuples(counts.get()) > 0;
    if (countsAvailable) {
        openCount = cellInt(counts.get(), 0, 0);
        actionRequiredCount = cellInt(counts.get(), 0, 1);
    } else {
        markUnavailable(freshness, "tradeCounts");
    }

    auto result = execute(
        connection,
        "SELECT id, offered_by_email, offered_to_email, status, requires_approval, "
        "array_to_json(offered_player_ids)::text, array_to_json(requested_player_ids)::text, "
        "to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') "
        "FROM trade_offers WHERE league_id = $1 AND status IN ('pending', 'accepted') "
        "AND (lower(offered_by_email) = lower($2) OR lower(offered_to_email) = lower($2) "
        " OR ($3 = 'true' AND status = 'accepted' AND requires_approval)) "
        "ORDER BY created_at DESC LIMIT 5",
        parameters);
    Json::Value payload(Json::objectValue);
    Json::Value items(Json::arrayValue);
    if (!tuplesOk(result.get())) {
        markUnavailable(freshness, "trades");
        payload["available"] = false;
        payload["actionRequiredCount"] = actionRequiredCount;
        payload["openCount"] = openCount;
        payload["items"] = items;
        return payload;
    }

    int displayedActionRequired = 0;
    for (int row = 0; row < PQntuples(result.get()); ++row) {
        const auto recipient = canonical(cell(result.get(), row, 2)) == canonical(actorEmail);
        const auto approval = commissioner && cellBool(result.get(), row, 4) && cell(result.get(), row, 3) == "accepted";
        const auto actionRequired = (recipient && cell(result.get(), row, 3) == "pending") || approval;
        if (actionRequired) ++displayedActionRequired;
        Json::Value item(Json::objectValue);
        item["id"] = cell(result.get(), row, 0);
        item["offeredBy"] = cell(result.get(), row, 1);
        item["offeredTo"] = cell(result.get(), row, 2);
        item["status"] = cell(result.get(), row, 3);
        item["requiresApproval"] = cellBool(result.get(), row, 4);
        item["offeredPlayerIds"] = parseJson(cell(result.get(), row, 5), Json::Value(Json::arrayValue));
        item["requestedPlayerIds"] = parseJson(cell(result.get(), row, 6), Json::Value(Json::arrayValue));
        item["createdAt"] = cell(result.get(), row, 7);
        item["actionRequired"] = actionRequired;
        items.append(item);
    }
    if (!countsAvailable) {
        openCount = static_cast<int>(items.size());
        actionRequiredCount = displayedActionRequired;
    }
    payload["available"] = true;
    payload["actionRequiredCount"] = actionRequiredCount;
    payload["openCount"] = openCount;
    payload["items"] = items;
    return payload;
}

Json::Value loadStandings(
    PGconn *connection,
    const std::string &leagueId,
    const std::string &actorEmail,
    Json::Value &freshness) {
    auto result = execute(
        connection,
        "SELECT s.season, s.rank, s.manager_email, COALESCE(m.team_name, s.manager_email), "
        "s.wins, s.losses, s.ties, s.points_for::text, s.points_against::text "
        "FROM league_standings s "
        "LEFT JOIN league_members m ON m.league_id = s.league_id "
        " AND lower(m.email) = lower(s.manager_email) "
        "WHERE s.league_id = $1 AND s.season = (SELECT MAX(season) FROM league_standings WHERE league_id = $1) "
        "ORDER BY s.rank, lower(s.manager_email)",
        {leagueId});
    Json::Value payload(Json::objectValue);
    Json::Value leaders(Json::arrayValue);
    if (!tuplesOk(result.get())) {
        markUnavailable(freshness, "standings");
        payload["available"] = false;
        payload["leaders"] = leaders;
        return payload;
    }

    Json::Value actor(Json::nullValue);
    for (int row = 0; row < PQntuples(result.get()); ++row) {
        Json::Value item(Json::objectValue);
        item["season"] = cellInt(result.get(), row, 0);
        item["rank"] = cellInt(result.get(), row, 1);
        item["managerEmail"] = cell(result.get(), row, 2);
        item["teamName"] = cell(result.get(), row, 3);
        item["wins"] = cellInt(result.get(), row, 4);
        item["losses"] = cellInt(result.get(), row, 5);
        item["ties"] = cellInt(result.get(), row, 6);
        item["pointsFor"] = cellDouble(result.get(), row, 7);
        item["pointsAgainst"] = cellDouble(result.get(), row, 8);
        if (leaders.size() < 5) leaders.append(item);
        if (canonical(item["managerEmail"].asString()) == canonical(actorEmail)) actor = item;
    }
    payload["available"] = true;
    payload["season"] = PQntuples(result.get()) > 0 ? cellInt(result.get(), 0, 0) : 0;
    payload["leaders"] = leaders;
    payload["myTeam"] = actor;
    return payload;
}

Json::Value loadActivity(
    PGconn *connection,
    const std::string &leagueId,
    Json::Value &freshness) {
    auto result = execute(
        connection,
        "SELECT kind, actor, summary, "
        "to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"') FROM ("
        " SELECT 'transaction' AS kind, COALESCE(manager_email, '') AS actor, summary, created_at "
        " FROM transactions WHERE league_id = $1 "
        " UNION ALL "
        " SELECT 'commissioner_post' AS kind, manager_email AS actor, body AS summary, created_at "
        " FROM league_feed_posts WHERE league_id = $1"
        ") feed ORDER BY created_at DESC LIMIT 8",
        {leagueId});
    Json::Value activity(Json::arrayValue);
    if (!tuplesOk(result.get())) {
        markUnavailable(freshness, "activity");
        return activity;
    }
    for (int row = 0; row < PQntuples(result.get()); ++row) {
        Json::Value item(Json::objectValue);
        item["type"] = cell(result.get(), row, 0);
        item["actor"] = cell(result.get(), row, 1);
        item["summary"] = cell(result.get(), row, 2);
        item["createdAt"] = cell(result.get(), row, 3);
        activity.append(item);
    }
    return activity;
}

Json::Value loadNotices(
    PGconn *connection,
    const Context &context,
    const std::string &leagueId,
    bool commissioner,
    Json::Value &freshness) {
    Json::Value notices(Json::arrayValue);
    if (!context.notes.empty()) {
        Json::Value note(Json::objectValue);
        note["type"] = "league_note";
        note["message"] = context.notes;
        notices.append(note);
    }
    if (!commissioner) return notices;

    auto result = execute(
        connection,
        "SELECT COUNT(*) FROM league_members WHERE league_id = $1 AND status IN ('pending', 'invited')",
        {leagueId});
    if (!tuplesOk(result.get())) {
        markUnavailable(freshness, "commissionerNotices");
        return notices;
    }
    const auto pending = cellInt(result.get(), 0, 0);
    if (pending > 0) {
        Json::Value notice(Json::objectValue);
        notice["type"] = "pending_members";
        notice["count"] = pending;
        notice["message"] = std::to_string(pending) + " manager invitation"
            + (pending == 1 ? " is" : "s are") + " waiting for action.";
        notice["href"] = "league.html#managers";
        notices.append(notice);
    }
    return notices;
}

Json::Value deadlinePayload(
    const Context &context,
    const std::string &pickDeadline,
    const std::string &lineupDeadline) {
    Json::Value deadlines(Json::arrayValue);
    const auto append = [&deadlines](const std::string &type, const std::string &label, const std::string &at) {
        if (at.empty()) return;
        Json::Value item(Json::objectValue);
        item["type"] = type;
        item["label"] = label;
        item["at"] = at;
        deadlines.append(item);
    };
    append("draft", "Scheduled draft", context.draftDate);
    append("pick", "Current pick deadline", pickDeadline);
    append("lineup", "Lineup deadline", lineupDeadline);
    if (context.waiverRules.isObject()) {
        append("waiver", "Waiver claim deadline", context.waiverRules.get("claimDeadline", "").asString());
    }
    return deadlines;
}

Json::Value nextActionPayload(const cff::league_dashboard::NextAction &action) {
    Json::Value payload(Json::objectValue);
    payload["code"] = action.code;
    payload["label"] = action.label;
    payload["href"] = action.href;
    payload["detail"] = action.detail;
    return payload;
}
