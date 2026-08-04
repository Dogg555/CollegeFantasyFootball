drogon::HttpResponsePtr tradeLifecycleAdvice(const drogon::HttpRequestPtr &request) {
#ifdef CFF_HAS_POSTGRES
    if (!dbConfigured()) return nullptr;
    const auto &path = request->getPath();
    const auto method = request->getMethod();
    std::string leagueId;
    std::string tradeId;

    enum class Route {
        None,
        Preflight,
        State,
        Transaction,
        List,
        Create,
        Status
    } route = Route::None;

    if (method == drogon::Options
        && path.rfind("/api/leagues/", 0) == 0
        && path.find("/trades") != std::string::npos) {
        route = Route::Preflight;
    } else if (method == drogon::Get
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
            response = getTradeState(leagueId, *email, false);
            break;
        case Route::Transaction:
            response = dispatchTradeTransaction(request, leagueId, *email);
            break;
        case Route::List:
            response = getTradeState(leagueId, *email, true);
            break;
        case Route::Create:
            response = createTradeOffer(request, leagueId, *email, true);
            break;
        case Route::Status:
            response = updateTradeLifecycleStatus(request, leagueId, *email, tradeId, true);
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

struct TradeLifecycleInstaller {
    TradeLifecycleInstaller() {
        drogon::app().registerSyncAdvice(tradeLifecycleAdvice);
    }
};

TradeLifecycleInstaller tradeLifecycleInstaller;
