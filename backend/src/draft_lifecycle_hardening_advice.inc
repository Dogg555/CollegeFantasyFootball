drogon::HttpResponsePtr draftLifecycleAdvice(const drogon::HttpRequestPtr &request) {
#ifdef CFF_HAS_POSTGRES
    if (!dbConfigured()) return nullptr;
    const auto &path = request->getPath();
    const auto method = request->getMethod();

    std::string leagueId;
    enum class Action { None, Get, ReadinessGet, ReadinessSet, Order, Start, Pick, Reset, Undo } action = Action::None;

    if (method == drogon::Get && !(leagueId = pathLeagueId(path, "/draft/readiness")).empty()) {
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
    const auto email = accountEmail(request);
    if (!email) {
        return errorResponse(drogon::k401Unauthorized,
                             "Authentication is required.",
                             "authentication_required");
    }

    switch (action) {
        case Action::Get:
        case Action::ReadinessGet:
            return getDraft(leagueId, *email);
        case Action::ReadinessSet:
            return setReadiness(request, leagueId, *email);
        case Action::Order:
            return saveOrder(request, leagueId, *email);
        case Action::Start:
            return startDraft(request, leagueId, *email);
        case Action::Pick:
            return makePick(request, leagueId, *email);
        case Action::Reset:
            return resetDraft(request, leagueId, *email);
        case Action::Undo:
            return undoPick(request, leagueId, *email);
        default:
            return nullptr;
    }
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
