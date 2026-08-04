drogon::HttpResponsePtr scheduleLineupAdvice(const drogon::HttpRequestPtr &request) {
#ifdef CFF_HAS_POSTGRES
    if (!dbConfigured()) return nullptr;
    const auto &path = request->getPath();
    const auto method = request->getMethod();
    std::string leagueId;
    std::string playerId;
    int week = 1;

    enum class Route {
        None,
        State,
        Transaction,
        LegacyGenerate,
        LegacyGenerateSeason,
        LineupState,
        LineupLock,
        LineupUnlock,
        RosterSlotGuard,
        RosterDropGuard,
        Preflight
    } route = Route::None;

    if (method == drogon::Get
        && !(leagueId = pathLeagueId(path, "/schedule/state")).empty()) {
        route = Route::State;
    } else if (method == drogon::Post
               && !(leagueId = pathLeagueId(path, "/schedule/transactions")).empty()) {
        route = Route::Transaction;
    } else if (method == drogon::Post
               && !(leagueId = pathLeagueId(path, "/matchups/generate-season")).empty()) {
        route = Route::LegacyGenerateSeason;
    } else if (method == drogon::Post
               && !(leagueId = pathLeagueId(path, "/matchups/generate")).empty()) {
        route = Route::LegacyGenerate;
    } else if (method == drogon::Get
               && parseLineupPath(path, "", leagueId, week)) {
        route = Route::LineupState;
    } else if (method == drogon::Post
               && parseLineupPath(path, "/lock", leagueId, week)) {
        route = Route::LineupLock;
    } else if (method == drogon::Post
               && parseLineupPath(path, "/unlock", leagueId, week)) {
        route = Route::LineupUnlock;
    } else if ((method == drogon::Post || method == drogon::Put)
               && parseRosterSlotPath(path, leagueId, playerId)) {
        route = Route::RosterSlotGuard;
    } else if (method == drogon::Post
               && !(leagueId = pathLeagueId(path, "/roster/drop")).empty()) {
        route = Route::RosterDropGuard;
    } else if (method == drogon::Options
               && (!(leagueId = pathLeagueId(path, "/schedule/state")).empty()
                   || !(leagueId = pathLeagueId(path, "/schedule/transactions")).empty()
                   || parseLineupPath(path, "", leagueId, week)
                   || parseLineupPath(path, "/lock", leagueId, week)
                   || parseLineupPath(path, "/unlock", leagueId, week))) {
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
        case Route::LineupState:
            return getScheduleState(leagueId, *email, requestSeason(request), requestWeek(request, week));
        case Route::Transaction:
            return dispatchScheduleTransaction(request, leagueId, *email);
        case Route::LegacyGenerate:
            return generateSchedule(request, leagueId, *email, requestSeason(request), 1, true);
        case Route::LegacyGenerateSeason: {
            const auto body = request->getJsonObject();
            const auto weeks = body && body->isObject()
                ? positiveInt(body->get("weeks", 12), 12)
                : 12;
            return generateSchedule(request, leagueId, *email, requestSeason(request), weeks, true);
        }
        case Route::LineupLock:
            return mutateLineupLock(request, leagueId, *email,
                                    requestSeason(request), week, false, true);
        case Route::LineupUnlock:
            return mutateLineupLock(request, leagueId, *email,
                                    requestSeason(request), week, true, true);
        case Route::RosterSlotGuard:
            return rosterSlotGuard(leagueId, *email);
        case Route::RosterDropGuard:
            return rosterDropGuard(request, leagueId, *email);
        default:
            return nullptr;
    }
#else
    (void)request;
    return nullptr;
#endif
}

struct ScheduleLineupInstaller {
    ScheduleLineupInstaller() {
        drogon::app().registerSyncAdvice(scheduleLineupAdvice);
    }
};

#if defined(__GNUC__)
__attribute__((init_priority(101)))
#endif
ScheduleLineupInstaller scheduleLineupInstaller;
