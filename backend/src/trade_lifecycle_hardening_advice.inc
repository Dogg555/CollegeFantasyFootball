drogon::HttpResponsePtr tradeLifecycleAdvice(const drogon::HttpRequestPtr &request) {
#ifdef CFF_HAS_POSTGRES
    if (!dbConfigured()) return nullptr;
    const auto &path = request->getPath();
    const auto method = request->getMethod();
    std::string leagueId;
    std::string tradeId;

    enum class Route {
        None,
        State,
        Transaction,
        List,
        Create,
        Status
    } route = Route::None;

    if (method == drogon::Get
        && !(leagueId = pathLeagueId(path, "/trades/state")).empty()) {
        route = Route::State;
    } else if (method == drogon::Post
               && !(leagueId = pathLeagueId(path, "/trades/transactions")).empty()) {
        route = Route::Transaction;
    } else if (method == drogon::Post
               && parseTradePath(path, "/status", leagueId, tradeId)) {
        route = Route::Status;
    } else if (method == drogon::Get
               && !(leagueId = pathLeagueId(path, "/trades")).empty()) {
        route = Route::List;
    } else if (method == drogon::Post
               && !(leagueId = pathLeagueId(path, "/trades")).empty()) {
        route = Route::Create;
    }

    if (route == Route::None) return nullptr;
    const auto respond = [&request](const drogon::HttpResponsePtr &response) {
        return cff::http::withRuntimeCorsHeaders(request, response);
    };
    const auto email = accountEmail(request);
    if (!email) {
        return respond(errorResponse(drogon::k401Unauthorized,
                                     "Authentication is required.",
                                     "authentication_required"));
    }

    switch (route) {
        case Route::State:
            return respond(getTradeState(leagueId, *email, false));
        case Route::Transaction:
            return respond(dispatchTradeTransaction(request, leagueId, *email));
        case Route::List:
            return respond(getTradeState(leagueId, *email, true));
        case Route::Create:
            return respond(createTradeOffer(request, leagueId, *email, true));
        case Route::Status:
            return respond(updateTradeLifecycleStatus(request, leagueId, *email, tradeId, true));
        default:
            return nullptr;
    }
#else
    (void)request;
    return nullptr;
#endif
}

struct TradeLifecycleInstaller {
    TradeLifecycleInstaller() {
        drogon::app().registerSyncAdvice(tradeLifecycleAdvice);
    }
};

TradeLifecycleInstaller tradeLifecycleInstaller;
