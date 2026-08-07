drogon::HttpResponsePtr rosterTransactionAdvice(const drogon::HttpRequestPtr &request) {
#ifdef CFF_HAS_POSTGRES
    if (!dbConfigured()) return nullptr;
    const auto &path = request->getPath();
    const auto method = request->getMethod();

    std::string leagueId;
    std::string playerId;
    enum class Action { None, Directory, State, Transaction, LegacyAdd, LegacyDrop, LegacySlot } action = Action::None;

    if (method == drogon::Get && !(leagueId = pathLeagueId(path, "/players")).empty()) {
        action = Action::Directory;
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
    const auto respond = [&request](const drogon::HttpResponsePtr &response) {
        return cff::http::withRuntimeCorsHeaders(request, response);
    };
    const auto email = accountEmail(request);
    if (!email) {
        return respond(errorResponse(drogon::k401Unauthorized,
                                     "Authentication is required.",
                                     "authentication_required"));
    }

    switch (action) {
        case Action::Directory:
            return respond(getFreeAgentDirectory(request, leagueId, *email));
        case Action::State:
            return respond(getRosterState(leagueId, *email));
        case Action::Transaction: {
            const auto body = request->getJsonObject();
            const auto requestedAction = body && body->isObject()
                ? lower(trim(jsonStringMember(*body, "action")))
                : std::string{};
            return respond(requestedAction == "lineup"
                ? mutateLineup(request, leagueId, *email)
                : dispatchRosterTransaction(request, leagueId, *email));
        }
        case Action::LegacyAdd:
            return respond(mutateRoster(request, leagueId, *email, RosterAction::Add, "", true));
        case Action::LegacyDrop:
            return respond(mutateRoster(request, leagueId, *email, RosterAction::Drop, "", true));
        case Action::LegacySlot:
            return respond(mutateRoster(request, leagueId, *email, RosterAction::Slot, playerId, true));
        default:
            return nullptr;
    }
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
