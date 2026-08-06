void handleDashboard(
    const drogon::HttpRequestPtr &request,
    std::function<void(const drogon::HttpResponsePtr &)> &&callback,
    const std::optional<std::string> &jwtSecret,
    const std::string &leagueId) {
    std::string actorEmail;
    if (!cff::http::requireAccount(request, callback, jwtSecret, actorEmail)) return;

    auto connection = connectToDatabase();
    if (!connection) {
        sendError(callback, drogon::k503ServiceUnavailable, "dashboard_unavailable",
                  "League dashboard data is temporarily unavailable.");
        return;
    }

    const auto context = loadContext(connection.get(), leagueId, actorEmail);
    if (!context) {
        sendError(callback, drogon::k404NotFound, "league_not_found",
                  "The league was not found or is not available to this account.");
        return;
    }

    const auto generatedAt = utcNow();
    Json::Value payload(Json::objectValue);
    Json::Value freshness(Json::objectValue);
    freshness["source"] = "api";
    freshness["generatedAt"] = generatedAt;
    freshness["serverTime"] = generatedAt;
    freshness["stale"] = false;
    freshness["partial"] = false;
    freshness["unavailableSections"] = Json::Value(Json::arrayValue);

    const auto commissioner = context->owner || context->role == "commissioner";
    std::string draftStatus;
    std::string pickDeadline;
    auto draft = loadDraft(connection.get(), *context, leagueId, freshness, draftStatus, pickDeadline);

    bool hasLineupWarnings = false;
    bool hasRoster = false;
    std::string lineupDeadline;
    auto lineup = loadLineup(
        connection.get(), *context, leagueId, actorEmail,
        canonical(draftStatus) == "complete", freshness,
        hasLineupWarnings, hasRoster, lineupDeadline);

    bool hasCurrentMatchup = false;
    auto currentMatchup = loadCurrentMatchup(
        connection.get(), leagueId, actorEmail, freshness, hasCurrentMatchup);

    int pendingWaivers = 0;
    auto waivers = loadWaivers(connection.get(), leagueId, actorEmail, freshness, pendingWaivers);

    int actionRequiredTrades = 0;
    auto trades = loadTrades(
        connection.get(), leagueId, actorEmail, commissioner, freshness, actionRequiredTrades);

    auto standings = loadStandings(connection.get(), leagueId, actorEmail, freshness);
    auto activity = loadActivity(connection.get(), leagueId, freshness);
    auto notices = loadNotices(connection.get(), *context, leagueId, commissioner, freshness);

    cff::league_dashboard::DashboardSignals signals;
    signals.commissioner = commissioner;
    signals.activeManagers = context->activeManagers;
    signals.teamCount = context->teamCount;
    signals.draftStatus = draftStatus;
    signals.hasLineupWarnings = hasLineupWarnings;
    signals.actionRequiredTrades = actionRequiredTrades;
    signals.pendingWaivers = pendingWaivers;
    signals.hasCurrentMatchup = hasCurrentMatchup;
    signals.hasRoster = hasRoster;

    payload["leagueId"] = leagueId;
    payload["actorEmail"] = canonical(actorEmail);
    payload["league"] = leaguePayload(*context, leagueId);
    payload["nextAction"] = nextActionPayload(cff::league_dashboard::chooseNextAction(signals));
    payload["draft"] = draft;
    payload["lineup"] = lineup;
    payload["currentMatchup"] = currentMatchup;
    payload["waivers"] = waivers;
    payload["trades"] = trades;
    payload["standings"] = standings;
    payload["activity"] = activity;
    payload["commissionerNotices"] = notices;
    payload["deadlines"] = deadlinePayload(*context, pickDeadline, lineupDeadline);
    payload["freshness"] = freshness;
    payload["readOnly"] = false;
    callback(jsonResponse(payload));
}
