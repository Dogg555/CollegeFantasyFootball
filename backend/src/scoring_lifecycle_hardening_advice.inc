drogon::HttpResponsePtr scoringLifecycleAdvice(const drogon::HttpRequestPtr &request) {
#ifdef CFF_HAS_POSTGRES
    if (!dbConfigured()) return nullptr;
    const auto &path = request->getPath();
    const auto method = request->getMethod();
    std::string leagueId;
    int week = 1;

    enum class Route {
        None,
        State,
        Standings,
        Transaction,
        LegacyScore,
        LegacyFinalize,
        Preflight
    } route = Route::None;

    if (method == drogon::Get
        && !(leagueId = pathLeagueId(path, "/scoring/state")).empty()) {
        route = Route::State;
    } else if (method == drogon::Get
               && !(leagueId = pathLeagueId(path, "/standings")).empty()) {
        route = Route::Standings;
    } else if (method == drogon::Post
               && !(leagueId = pathLeagueId(path, "/scoring/transactions")).empty()) {
        route = Route::Transaction;
    } else if (method == drogon::Post
               && parseScoreWeekPath(path, "/finalize", leagueId, week)) {
        route = Route::LegacyFinalize;
    } else if (method == drogon::Post
               && parseScoreWeekPath(path, "", leagueId, week)) {
        route = Route::LegacyScore;
    } else if (method == drogon::Options
               && (!(leagueId = pathLeagueId(path, "/scoring/state")).empty()
                   || !(leagueId = pathLeagueId(path, "/standings")).empty()
                   || !(leagueId = pathLeagueId(path, "/scoring/transactions")).empty())) {
        route = Route::Preflight;
    }

    if (route == Route::None) return nullptr;
    if (route == Route::Preflight) {
        const auto config = cff::config::loadRuntimeConfig();
        return cff::http::buildPreflightResponse(request, config.allowedOrigins);
    }

    const auto email = accountEmail(request);
    if (!email) {
        return errorResponse(drogon::k401Unauthorized,
                             "Authentication is required.",
                             "authentication_required");
    }

    switch (route) {
        case Route::State:
            return getScoringState(leagueId, *email, requestSeason(request), requestWeek(request));
        case Route::Standings:
            return getStandingsState(leagueId, *email, requestSeason(request));
        case Route::Transaction:
            return dispatchScoringTransaction(request, leagueId, *email);
        case Route::LegacyScore:
            return scoreWeek(request, leagueId, *email, requestSeason(request), week, true);
        case Route::LegacyFinalize:
            return finalizeWeek(request, leagueId, *email, requestSeason(request), week, true);
        default:
            return nullptr;
    }
#else
    (void)request;
    return nullptr;
#endif
}

struct ScoringLifecycleInstaller {
    ScoringLifecycleInstaller() {
        drogon::app().registerSyncAdvice(scoringLifecycleAdvice);
    }
};

ScoringLifecycleInstaller scoringLifecycleInstaller;
