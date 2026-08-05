drogon::HttpResponsePtr statIngestionAdvice(const drogon::HttpRequestPtr &request) {
#ifdef CFF_HAS_POSTGRES
    if (!dbConfigured()) return nullptr;
    const auto &path = request->getPath();
    const auto method = request->getMethod();
    const bool statusRoute = path == "/api/admin/ingest/cfbd/stats/status";
    const bool transactionRoute = path == "/api/admin/ingest/cfbd/stats/transactions";
    if (!statusRoute && !transactionRoute) return nullptr;
    const auto respond = [&request](const drogon::HttpResponsePtr &response) {
        return cff::http::withRuntimeCorsHeaders(request, response);
    };

    if (method == drogon::Options) {
        const auto config = cff::config::loadRuntimeConfig();
        return cff::http::buildPreflightResponse(request, config.allowedOrigins);
    }
    if ((statusRoute && method != drogon::Get)
        || (transactionRoute && method != drogon::Post)) {
        return respond(errorResponse(drogon::k405MethodNotAllowed,
                                     "Method not allowed.",
                                     "method_not_allowed"));
    }

    const auto config = cff::config::loadRuntimeConfig();
    std::string actor;
    if (!cff::http::bearerToken(request)) {
        return respond(errorResponse(drogon::k401Unauthorized,
                                     "Authentication is required.",
                                     "authentication_required"));
    }
    if (!cff::http::isAdminRequest(request, config.jwtSecret, actor)) {
        return respond(errorResponse(drogon::k403Forbidden,
                                     "Admin access is required.",
                                     "admin_required"));
    }
    actor = lower(trim(actor));
    const auto season = requestSeason(request);
    const auto week = requestWeek(request);
    if (statusRoute) return respond(getStatStatus(season, week, actor));

    const auto body = request->getJsonObject();
    const auto action = body && body->isObject()
        ? lower(trim(body->get("action", "").asString()))
        : std::string{};
    if (action == "recover") {
        const auto key = operationKey(request);
        if (!key.empty()) {
            auto context = openStatContext(season, week);
            if (!context) return respond(statStorageUnavailable());
            if (const auto replay = operationReplay(
                    context->connection.get(), season, week, actor, key, "recover")) {
                if (!(*replay)["operationTypeMatches"].asBool()) {
                    rollback(context->connection.get());
                    return respond(errorResponse(drogon::k409Conflict,
                                                 "This idempotency key was used for another ingestion action.",
                                                 "idempotency_key_conflict"));
                }
                auto payload = *replay;
                payload.removeMember("operationTypeMatches");
                payload.removeMember("storedOperationType");
                if (!commit(context->connection.get())) return respond(statStorageUnavailable());
                return respond(jsonResponse(payload));
            }
            rollback(context->connection.get());
        }
    }
    return respond(dispatchStatTransaction(request, season, week, actor));
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
