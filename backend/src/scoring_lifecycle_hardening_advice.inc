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

    if (method == drogon::Options
        && path.rfind("/api/leagues/", 0) == 0
        && (path.find("/scoring") != std::string::npos
            || path.find("/standings") != std::string::npos
            || path.find("/score/week/") != std::string::npos)) {
        route = Route::Preflight;
    } else if (method == drogon::Get
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
    }

    if (route == Route::None) return nullptr;
    if (route == Route::Preflight) {
        const auto config = cff::config::loadRuntimeConfig();
        return cff::http::buildPreflightResponse(request, config.allowedOrigins);
    }

    const auto email = accountEmail(request);
    if (!email) {
        return cff::http::withConfiguredCorsHeaders(
            request,
            errorResponse(drogon::k401Unauthorized,
                          "Authentication is required.",
                          "authentication_required"));
    }

    drogon::HttpResponsePtr response;
    switch (route) {
        case Route::State:
            response = getScoringState(leagueId, *email, requestSeason(request), requestWeek(request));
            break;
        case Route::Standings:
            response = getStandingsState(leagueId, *email, requestSeason(request));
            break;
        case Route::Transaction:
            response = dispatchScoringTransaction(request, leagueId, *email);
            break;
        case Route::LegacyScore:
            response = scoreWeek(request, leagueId, *email, requestSeason(request), week, true);
            break;
        case Route::LegacyFinalize:
            response = finalizeWeek(request, leagueId, *email, requestSeason(request), week, true);
            break;
        default:
            return nullptr;
    }
    return cff::http::withConfiguredCorsHeaders(request, response);
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
