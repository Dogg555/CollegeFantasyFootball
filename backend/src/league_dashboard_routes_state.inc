Json::Value loadDraft(
    PGconn *connection,
    const Context &context,
    const std::string &leagueId,
    Json::Value &freshness,
    std::string &status,
    std::string &pickDeadline) {
    auto result = execute(
        connection,
        "SELECT COALESCE(d.status, 'not_started'), COALESCE(d.current_pick, 1), "
        "COALESCE(to_char(d.pick_deadline AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), ''), "
        "(SELECT COUNT(*) FROM draft_picks p WHERE p.league_id = $1) "
        "FROM (SELECT 1) seed LEFT JOIN draft_states d ON d.league_id = $1",
        {leagueId});

    Json::Value draft(Json::objectValue);
    if (!tuplesOk(result.get()) || PQntuples(result.get()) == 0) {
        markUnavailable(freshness, "draft");
        status = context.draftLobbyOpen ? "lobby_open" : "not_started";
        draft["status"] = status;
        draft["available"] = false;
        return draft;
    }

    status = cell(result.get(), 0, 0);
    pickDeadline = cell(result.get(), 0, 2);
    const auto picksMade = cellInt(result.get(), 0, 3);
    int rosterSize = 0;
    if (context.rosterRules.isObject()) {
        for (const auto &key : context.rosterRules.getMemberNames()) {
            rosterSize += std::max(0, context.rosterRules[key].asInt());
        }
    }
    draft["status"] = status;
    draft["currentPick"] = cellInt(result.get(), 0, 1, 1);
    draft["pickDeadline"] = pickDeadline;
    draft["picksMade"] = picksMade;
    draft["expectedPicks"] = rosterSize * context.activeManagers;
    draft["progress"] = rosterSize > 0 && context.activeManagers > 0
        ? static_cast<double>(picksMade) / static_cast<double>(rosterSize * context.activeManagers)
        : 0.0;
    draft["available"] = true;
    return draft;
}

Json::Value loadLineup(
    PGconn *connection,
    const Context &context,
    const std::string &leagueId,
    const std::string &actorEmail,
    bool draftComplete,
    Json::Value &freshness,
    bool &hasWarnings,
    bool &hasRoster,
    std::string &lineupDeadline) {
    std::map<std::string, int> required;
    if (context.rosterRules.isObject()) {
        for (const auto &key : context.rosterRules.getMemberNames()) {
            required[canonical(key)] = std::max(0, context.rosterRules[key].asInt());
        }
    }

    auto result = execute(
        connection,
        "SELECT lower(roster_slot), COUNT(*) FROM rosters "
        "WHERE league_id = $1 AND lower(manager_email) = lower($2) "
        "GROUP BY lower(roster_slot)",
        {leagueId, actorEmail});
    std::map<std::string, int> assigned;
    int rosterCount = 0;
    if (!tuplesOk(result.get())) {
        markUnavailable(freshness, "lineup");
    } else {
        for (int row = 0; row < PQntuples(result.get()); ++row) {
            assigned[canonical(cell(result.get(), row, 0))] = cellInt(result.get(), row, 1);
            rosterCount += cellInt(result.get(), row, 1);
        }
    }
    hasRoster = rosterCount > 0;

    Json::Value warnings(Json::arrayValue);
    const auto missing = cff::league_dashboard::missingStarterWarnings(required, assigned, draftComplete);
    for (const auto &warning : missing) {
        Json::Value item(Json::objectValue);
        item["code"] = "missing_starter";
        item["slot"] = warning.slot;
        item["missing"] = warning.missing;
        item["message"] = "Missing " + std::to_string(warning.missing) + " " + warning.slot + " starter"
            + (warning.missing == 1 ? "" : "s") + ".";
        warnings.append(item);
    }
    hasWarnings = !missing.empty();

    Json::Value lineup(Json::objectValue);
    lineup["rosterCount"] = rosterCount;
    lineup["warnings"] = warnings;
    int missingStarterCount = 0;
    for (const auto &warning : missing) missingStarterCount += warning.missing;
    lineup["missingStarterCount"] = missingStarterCount;
    lineup["status"] = !draftComplete ? "pre_draft" : hasWarnings ? "incomplete" : "ready";

    auto week = execute(
        connection,
        "SELECT schedule.season, schedule.week, "
        "COALESCE(lineup.status, schedule.status), "
        "COALESCE(to_char(schedule.lineup_deadline AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), '') "
        "FROM schedule_week_states schedule "
        "LEFT JOIN lineup_week_states lineup ON lineup.league_id = schedule.league_id "
        " AND lineup.season = schedule.season AND lineup.week = schedule.week "
        " AND lower(lineup.manager_email) = lower($2) "
        "WHERE schedule.league_id = $1 AND schedule.status <> 'finalized' "
        "ORDER BY schedule.season DESC, schedule.week ASC LIMIT 1",
        {leagueId, actorEmail});
    if (tuplesOk(week.get()) && PQntuples(week.get()) > 0) {
        lineup["season"] = cellInt(week.get(), 0, 0);
        lineup["week"] = cellInt(week.get(), 0, 1);
        lineup["lockStatus"] = cell(week.get(), 0, 2);
        lineupDeadline = cell(week.get(), 0, 3);
        lineup["deadline"] = lineupDeadline;
    } else if (!tuplesOk(week.get())) {
        markUnavailable(freshness, "lineupDeadline");
    }
    return lineup;
}

Json::Value loadCurrentMatchup(
    PGconn *connection,
    const std::string &leagueId,
    const std::string &actorEmail,
    Json::Value &freshness,
    bool &hasMatchup) {
    auto result = execute(
        connection,
        "SELECT x.id, x.season, x.week, x.home_manager_email, COALESCE(x.away_manager_email, ''), "
        "x.home_score::text, x.away_score::text, x.status, "
        "COALESCE(home.team_name, x.home_manager_email), "
        "COALESCE(away.team_name, x.away_manager_email, 'Bye') "
        "FROM league_matchups x "
        "LEFT JOIN league_members home ON home.league_id = x.league_id "
        " AND lower(home.email) = lower(x.home_manager_email) "
        "LEFT JOIN league_members away ON away.league_id = x.league_id "
        " AND lower(away.email) = lower(x.away_manager_email) "
        "WHERE x.league_id = $1 AND (lower(x.home_manager_email) = lower($2) "
        " OR lower(COALESCE(x.away_manager_email, '')) = lower($2)) "
        "ORDER BY x.season DESC, CASE WHEN x.status = 'final' THEN 1 ELSE 0 END, "
        "CASE WHEN x.status <> 'final' THEN x.week END ASC, "
        "CASE WHEN x.status = 'final' THEN x.week END DESC LIMIT 1",
        {leagueId, actorEmail});
    if (!tuplesOk(result.get())) {
        markUnavailable(freshness, "matchup");
        return Json::Value(Json::nullValue);
    }
    if (PQntuples(result.get()) == 0) return Json::Value(Json::nullValue);

    const auto homeEmail = canonical(cell(result.get(), 0, 3));
    const auto actorIsHome = homeEmail == canonical(actorEmail);
    Json::Value matchup(Json::objectValue);
    matchup["id"] = cell(result.get(), 0, 0);
    matchup["season"] = cellInt(result.get(), 0, 1);
    matchup["week"] = cellInt(result.get(), 0, 2);
    matchup["status"] = cell(result.get(), 0, 7);
    matchup["isHome"] = actorIsHome;
    matchup["opponentEmail"] = actorIsHome ? cell(result.get(), 0, 4) : cell(result.get(), 0, 3);
    matchup["opponentTeamName"] = actorIsHome ? cell(result.get(), 0, 9) : cell(result.get(), 0, 8);
    matchup["scoreFor"] = actorIsHome ? cellDouble(result.get(), 0, 5) : cellDouble(result.get(), 0, 6);
    matchup["scoreAgainst"] = actorIsHome ? cellDouble(result.get(), 0, 6) : cellDouble(result.get(), 0, 5);
    hasMatchup = true;
    return matchup;
}
