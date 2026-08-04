drogon::HttpResponsePtr rosterTransactionAdvice(const drogon::HttpRequestPtr &request) {
#ifdef CFF_HAS_POSTGRES
    if (!dbConfigured()) return nullptr;
    const auto &path = request->getPath();
    const auto method = request->getMethod();

    std::string leagueId;
    std::string playerId;
    enum class Action {
        None,
        Preflight,
        State,
        Transaction,
        LegacyAdd,
        LegacyDrop,
        LegacySlot
    } action = Action::None;

    if (method == drogon::Options
        && path.rfind("/api/leagues/", 0) == 0
        && path.find("/roster") != std::string::npos) {
        action = Action::Preflight;
    } else if (method == drogon::Get && !(leagueId = pathLeagueId(path, "/roster/state")).empty()) {
        action = Action::State;
    } else if (method == drogon::Post
               && !(leagueId = pathLeagueId(path, "/roster/transactions")).empty()) {
        action = Action::Transaction;
    } else if (method == drogon::Post
               && !(leagueId = pathLeagueId(path, "/roster/drop")).empty()) {
        action = Action::LegacyDrop;
    } else if ((method == drogon::Post || method == drogon::Put)
               && parseRosterSlotPath(path, leagueId, playerId)) {
        action = Action::LegacySlot;
    } else if (method == drogon::Post
               && !(leagueId = pathLeagueId(path, "/roster")).empty()) {
        action = Action::LegacyAdd;
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
        case Action::State:
            response = getRosterState(leagueId, *email);
            break;
        case Action::Transaction:
            response = dispatchRosterTransaction(request, leagueId, *email);
            break;
        case Action::LegacyAdd:
            response = mutateRoster(request, leagueId, *email, RosterAction::Add, "", true);
            break;
        case Action::LegacyDrop:
            response = mutateRoster(request, leagueId, *email, RosterAction::Drop, "", true);
            break;
        case Action::LegacySlot:
            response = mutateRoster(request, leagueId, *email, RosterAction::Slot, playerId, true);
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

struct RosterTransactionInstaller {
    RosterTransactionInstaller() {
        drogon::app().registerSyncAdvice(rosterTransactionAdvice);
    }
};

RosterTransactionInstaller rosterTransactionInstaller;
