drogon::HttpResponsePtr legacyWaiverCollection(const std::string &leagueId,
                                               const std::string &email,
                                               bool priorityOnly) {
#ifdef CFF_HAS_POSTGRES
    auto context = openWaiverContext(leagueId, email);
    if (!context) return waiverStorageUnavailable();
    if (!context->access.exists) {
        rollback(context->connection.get());
        return errorResponse(drogon::k404NotFound, "League not found.", "league_not_found");
    }
    if (!context->access.member) {
        rollback(context->connection.get());
        return errorResponse(drogon::k403Forbidden,
                             "Active league membership is required.",
                             "league_membership_required");
    }
    auto payload = waiverStatePayload(context->connection.get(), leagueId, email,
                                      context->access, context->commissioner, context->version);
    if (!commit(context->connection.get())) {
        rollback(context->connection.get());
        return waiverStorageUnavailable();
    }
    return jsonResponse(priorityOnly ? payload["priority"] : payload["claims"]);
#else
    (void)leagueId;
    (void)email;
    (void)priorityOnly;
    return nullptr;
#endif
}

drogon::HttpResponsePtr waiverLifecycleAdvice(const drogon::HttpRequestPtr &request) {
#ifdef CFF_HAS_POSTGRES
    if (!dbConfigured()) return nullptr;
    const auto &path = request->getPath();
    const auto method = request->getMethod();
    std::string leagueId;
    std::string claimId;

    enum class Route {
        None,
        Preflight,
        State,
        Transaction,
        List,
        Priority,
        Create,
        Cancel,
        Reorder,
        ProcessOne,
        ProcessAll,
        ResetPriority
    } route = Route::None;

    if (method == drogon::Options
        && path.rfind("/api/leagues/", 0) == 0
        && path.find("/waiver") != std::string::npos) {
        route = Route::Preflight;
    } else if (method == drogon::Get && !(leagueId = pathLeagueId(path, "/waivers/state")).empty()) {
        route = Route::State;
    } else if (method == drogon::Post
               && !(leagueId = pathLeagueId(path, "/waivers/transactions")).empty()) {
        route = Route::Transaction;
    } else if (method == drogon::Get
               && !(leagueId = pathLeagueId(path, "/waiver-priority")).empty()) {
        route = Route::Priority;
    } else if (method == drogon::Post
               && !(leagueId = pathLeagueId(path, "/waiver-priority/reset")).empty()) {
        route = Route::ResetPriority;
    } else if (method == drogon::Post
               && !(leagueId = pathLeagueId(path, "/waivers/process")).empty()) {
        route = Route::ProcessAll;
    } else if (method == drogon::Post
               && !(leagueId = pathLeagueId(path, "/waivers/reorder")).empty()) {
        route = Route::Reorder;
    } else if (method == drogon::Post
               && parseWaiverClaimPath(path, "/process", leagueId, claimId)) {
        route = Route::ProcessOne;
    } else if (method == drogon::Post
               && parseWaiverClaimPath(path, "/status", leagueId, claimId)) {
        route = Route::Cancel;
    } else if (method == drogon::Get
               && !(leagueId = pathLeagueId(path, "/waivers")).empty()) {
        route = Route::List;
    } else if (method == drogon::Post
               && !(leagueId = pathLeagueId(path, "/waivers")).empty()) {
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
            response = getWaiverState(leagueId, *email);
            break;
        case Route::Transaction:
            response = dispatchWaiverTransaction(request, leagueId, *email);
            break;
        case Route::List:
            response = legacyWaiverCollection(leagueId, *email, false);
            break;
        case Route::Priority:
            response = legacyWaiverCollection(leagueId, *email, true);
            break;
        case Route::Create:
            response = createWaiverClaim(request, leagueId, *email, true);
            break;
        case Route::Cancel: {
            const auto body = request->getJsonObject();
            const auto status = body && body->isObject()
                ? lower(trim(body->get("status", "").asString()))
                : "";
            if (status != "cancelled" && status != "canceled") {
                response = errorResponse(
                    drogon::k400BadRequest,
                    "Only cancellation is supported for pending waiver claims.",
                    "invalid_waiver_status");
                break;
            }
            response = cancelWaiverClaim(request, leagueId, *email, claimId, true);
            break;
        }
        case Route::Reorder:
            response = reorderWaiverClaims(request, leagueId, *email, true);
            break;
        case Route::ProcessOne:
            response = processWaiverClaims(request, leagueId, *email, claimId, false, true);
            break;
        case Route::ProcessAll:
            response = processWaiverClaims(request, leagueId, *email, "", true, true);
            break;
        case Route::ResetPriority:
            response = resetWaiverPriority(request, leagueId, *email, true);
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

struct WaiverLifecycleInstaller {
    WaiverLifecycleInstaller() {
        drogon::app().registerSyncAdvice(waiverLifecycleAdvice);
    }
};

WaiverLifecycleInstaller waiverLifecycleInstaller;
