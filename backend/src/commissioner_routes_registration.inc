void registerCommissionerRoutes(
    drogon::HttpAppFramework &app,
    const std::optional<std::string> &jwtSecret,
    const std::unordered_set<std::string> &allowedOrigins) {
    app.registerHandler(
        "/api/leagues/{1}/commissioner",
        [jwtSecret](const drogon::HttpRequestPtr &request,
                    std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                    const std::string &leagueId) {
            handleDashboard(request, std::move(callback), jwtSecret, leagueId);
        },
        {drogon::Get});

    app.registerHandler(
        "/api/leagues/{1}/commissioner/invitations",
        [jwtSecret](const drogon::HttpRequestPtr &request,
                    std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                    const std::string &leagueId) {
            handleInvite(request, std::move(callback), jwtSecret, leagueId);
        },
        {drogon::Post});

    app.registerHandler(
        "/api/leagues/{1}/commissioner/members/{2}/{3}",
        [jwtSecret](const drogon::HttpRequestPtr &request,
                    std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                    const std::string &leagueId,
                    const std::string &memberEmail,
                    const std::string &action) {
            handleMemberAction(request, std::move(callback), jwtSecret, leagueId, memberEmail, action);
        },
        {drogon::Post});

    app.registerHandler(
        "/api/leagues/{1}/commissioner",
        [allowedOrigins](const drogon::HttpRequestPtr &request,
                         std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                         const std::string &) {
            callback(cff::http::buildPreflightResponse(request, allowedOrigins));
        },
        {drogon::Options});
    app.registerHandler(
        "/api/leagues/{1}/commissioner/invitations",
        [allowedOrigins](const drogon::HttpRequestPtr &request,
                         std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                         const std::string &) {
            callback(cff::http::buildPreflightResponse(request, allowedOrigins));
        },
        {drogon::Options});
    app.registerHandler(
        "/api/leagues/{1}/commissioner/members/{2}/{3}",
        [allowedOrigins](const drogon::HttpRequestPtr &request,
                         std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                         const std::string &,
                         const std::string &,
                         const std::string &) {
            callback(cff::http::buildPreflightResponse(request, allowedOrigins));
        },
        {drogon::Options});
}

namespace {
struct CommissionerRouteInstaller {
    CommissionerRouteInstaller() {
        const auto config = cff::config::loadRuntimeConfig();
        registerCommissionerRoutes(drogon::app(), config.jwtSecret, config.allowedOrigins);
    }
};

CommissionerRouteInstaller commissionerRouteInstaller;
} // namespace
