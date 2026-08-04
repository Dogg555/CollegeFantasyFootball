drogon::HttpResponsePtr draftLifecycleAdvice(const drogon::HttpRequestPtr &request) {
#ifdef CFF_HAS_POSTGRES
    if (!dbConfigured()) return nullptr;
    const auto &path = request->getPath();
    const auto method = request->getMethod();

    std::string leagueId;
    enum class Action {
        None,
        Preflight,
        Get,
        ReadinessGet,
        ReadinessSet,
        Order,
        Start,
        Pick,
        Reset,
        Undo
    } action = Action::None;

    if (method == drogon::Options
        && path.rfind("/api/leagues/", 0) == 0
        && path.find("/draft") != std::string::npos) {
        action = Action::Preflight;
    } else if (method == drogon::Get && !(leagueId = pathLeagueId(path, "/draft/readiness")).empty()) {
        action = Action::ReadinessGet;
    } else if (method == drogon::Post && !(leagueId = pathLeagueId(path, "/draft/readiness")).empty()) {
        action = Action::ReadinessSet;
    } else if (method == drogon::Get && !(leagueId = pathLeagueId(path, "/draft")).empty()) {
        action = Action::Get;
    } else if ((method == drogon::Put || method == drogon::Post)
               && !(leagueId = pathLeagueId(path, "/draft/order")).empty()) {
        action = Action::Order;
    } else if (method == drogon::Post && !(leagueId = pathLeagueId(path, "/draft/start")).empty()) {
        action = Action::Start;
    } else if (method == drogon::Post && !(leagueId = pathLeagueId(path, "/draft/picks")).empty()) {
        action = Action::Pick;
    } else if (method == drogon::Post && !(leagueId = pathLeagueId(path, "/draft/reset")).empty()) {
        action = Action::Reset;
    } else if (method == drogon::Post && !(leagueId = pathLeagueId(path, "/draft/undo")).empty()) {
        action = Action::Undo;
    }

    if (action == Action::None) return nullptr;
    if (action == Action::Preflight) {
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
    switch (action) {
        case Action::Get:
        case Action::ReadinessGet:
            response = getDraft(leagueId, *email);
            break;
        case Action::ReadinessSet:
            response = setReadiness(request, leagueId, *email);
            break;
        case Action::Order:
            response = saveOrder(request, leagueId, *email);
            break;
        case Action::Start:
            response = startDraft(request, leagueId, *email);
            break;
        case Action::Pick:
            response = makePick(request, leagueId, *email);
            break;
        case Action::Reset:
            response = resetDraft(request, leagueId, *email);
            break;
        case Action::Undo:
            response = undoPick(request, leagueId, *email);
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

struct DraftLifecycleInstaller {
    DraftLifecycleInstaller() {
        drogon::app().registerSyncAdvice(draftLifecycleAdvice);
    }
};

DraftLifecycleInstaller draftLifecycleInstaller;
