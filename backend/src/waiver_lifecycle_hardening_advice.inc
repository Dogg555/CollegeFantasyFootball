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

    if (method == drogon::Get && !(leagueId = pathLeagueId(path, "/waivers/state")).empty()) {
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
            return respond(getWaiverState(leagueId, *email));
        case Route::Transaction:
            return respond(dispatchWaiverTransaction(request, leagueId, *email));
        case Route::List:
            return respond(legacyWaiverCollection(leagueId, *email, false));
        case Route::Priority:
            return respond(legacyWaiverCollection(leagueId, *email, true));
        case Route::Create:
            return respond(createWaiverClaim(request, leagueId, *email, true));
        case Route::Cancel: {
            const auto body = request->getJsonObject();
            const auto status = body && body->isObject()
                ? lower(trim(body->get("status", "").asString()))
                : "";
            if (status != "cancelled" && status != "canceled") {
                return respond(errorResponse(drogon::k400BadRequest,
                                             "Only cancellation is supported for pending waiver claims.",
                                             "invalid_waiver_status"));
            }
            return respond(cancelWaiverClaim(request, leagueId, *email, claimId, true));
        }
        case Route::Reorder:
            return respond(reorderWaiverClaims(request, leagueId, *email, true));
        case Route::ProcessOne:
            return respond(processWaiverClaims(request, leagueId, *email, claimId, false, true));
        case Route::ProcessAll:
            return respond(processWaiverClaims(request, leagueId, *email, "", true, true));
        case Route::ResetPriority:
            return respond(resetWaiverPriority(request, leagueId, *email, true));
        default:
            return nullptr;
    }
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
