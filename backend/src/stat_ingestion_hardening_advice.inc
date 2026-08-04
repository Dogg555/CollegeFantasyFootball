drogon::HttpResponsePtr statIngestionAdvice(const drogon::HttpRequestPtr &request) {
#ifdef CFF_HAS_POSTGRES
    if (!dbConfigured()) return nullptr;
    const auto &path = request->getPath();
    const auto method = request->getMethod();
    const bool statusRoute = path == "/api/admin/ingest/cfbd/stats/status";
    const bool transactionRoute = path == "/api/admin/ingest/cfbd/stats/transactions";
    if (!statusRoute && !transactionRoute) return nullptr;

    if (method == drogon::Options) {
        const auto config = cff::config::loadRuntimeConfig();
        return cff::http::buildPreflightResponse(request, config.allowedOrigins);
    }
    if ((statusRoute && method != drogon::Get)
        || (transactionRoute && method != drogon::Post)) {
        return errorResponse(drogon::k405MethodNotAllowed,
                             "Method not allowed.",
                             "method_not_allowed");
    }

    const auto config = cff::config::loadRuntimeConfig();
    std::string actor;
    if (!cff::http::bearerToken(request)) {
        return errorResponse(drogon::k401Unauthorized,
                             "Authentication is required.",
                             "authentication_required");
    }
    if (!cff::http::isAdminRequest(request, config.jwtSecret, actor)) {
        return errorResponse(drogon::k403Forbidden,
                             "Admin access is required.",
                             "admin_required");
    }
    actor = lower(trim(actor));
    const auto season = requestSeason(request);
    const auto week = requestWeek(request);
    if (statusRoute) return getStatStatus(season, week, actor);
    return dispatchStatTransaction(request, season, week, actor);
#else
    (void)request;
    return nullptr;
#endif
}

struct StatIngestionInstaller {
    StatIngestionInstaller() {
        drogon::app().registerSyncAdvice(statIngestionAdvice);
    }
};

StatIngestionInstaller statIngestionInstaller;
