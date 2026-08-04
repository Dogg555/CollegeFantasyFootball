drogon::HttpResponsePtr scheduleLifecycleAdvice(const drogon::HttpRequestPtr &request) {
#ifdef CFF_HAS_POSTGRES
    if (!dbConfigured()) return nullptr;
    const auto &path = request->getPath();
    const auto method = request->getMethod();
    std::string leagueId;
    int week = 1;

    enum class Route {
        None,
        State,
        Transaction,
        LegacyGenerateWeek,
        LegacyGenerateSeason,
        WeekState,
        Lock,
        Unlock,
        Deadline,
        MutationGuard,
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
        route = Route::LegacyGenerateWeek;
    } else if (method == drogon::Get && parseWeekPath(path, "", leagueId, week)) {
        route = Route::WeekState;
    } else if (method == drogon::Post && parseWeekPath(path, "/lock", leagueId, week)) {
        route = Route::Lock;
    } else if (method == drogon::Post && parseWeekPath(path, "/unlock", leagueId, week)) {
        route = Route::Unlock;
    } else if (method == drogon::Post && parseWeekPath(path, "/deadline", leagueId, week)) {
        route = Route::Deadline;
    } else if (method == drogon::Options
               && (!(leagueId = pathLeagueId(path, "/schedule/state")).empty()
                   || !(leagueId = pathLeagueId(path, "/schedule/transactions")).empty()
                   || !(leagueId = pathLeagueId(path, "/matchups/generate-season")).empty()
                   || !(leagueId = pathLeagueId(path, "/matchups/generate")).empty()
                   || parseWeekPath(path, "", leagueId, week)
                   || parseWeekPath(path, "/lock", leagueId, week)
                   || parseWeekPath(path, "/unlock", leagueId, week)
                   || parseWeekPath(path, "/deadline", leagueId, week))) {
        route = Route::Preflight;
    } else if (method == drogon::Post || method == drogon::Put || method == drogon::Delete) {
        const std::string prefix = "/api/leagues/";
        if (path.rfind(prefix, 0) == 0) {
            const auto separator = path.find('/', prefix.size());
            if (separator != std::string::npos) {
                leagueId = path.substr(prefix.size(), separator - prefix.size());
                const auto resource = path.substr(separator + 1);
                if (resource == "roster" || resource.rfind("roster/", 0) == 0
                    || resource == "waivers" || resource.rfind("waivers/", 0) == 0
                    || resource == "trades" || resource.rfind("trades/", 0) == 0
                    || resource == "draft/picks") {
                    route = Route::MutationGuard;
                }
            }
        }
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
        case Route::WeekState:
            return getScheduleState(leagueId, *email, requestSeason(request));
        case Route::Transaction:
            return dispatchScheduleTransaction(request, leagueId, *email);
        case Route::LegacyGenerateWeek:
            return generateSchedule(request, leagueId, *email, true, 1);
        case Route::LegacyGenerateSeason: {
            const auto body = request->getJsonObject();
            const auto weeks = body && body->isObject()
                ? std::clamp(positiveInt(body->get("weeks", 12), 12), 1, 18)
                : 12;
            return generateSchedule(request, leagueId, *email, true, weeks);
        }
        case Route::Lock:
            return mutateLineupControl(request, leagueId, *email, week, "lock", true);
        case Route::Unlock:
            return mutateLineupControl(request, leagueId, *email, week, "unlock", true);
        case Route::Deadline:
            return mutateLineupControl(request, leagueId, *email, week, "set_deadline", true);
        case Route::MutationGuard:
            return lineupMutationGuard(request, leagueId, *email);
        default:
            return nullptr;
    }
#else
    (void)request;
    return nullptr;
#endif
}

struct ScheduleLifecycleInstaller {
    ScheduleLifecycleInstaller() {
        drogon::AdviceCallbackSync advice = [](const drogon::HttpRequestPtr &request) {
            return scheduleLifecycleAdvice(request);
        };
        drogon::app().registerSyncAdvice(std::move(advice));
    }
};

ScheduleLifecycleInstaller scheduleLifecycleInstaller;