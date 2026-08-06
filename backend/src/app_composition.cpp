#include "app_composition.h"

#include "auth_routes.h"
#include "health_routes.h"
#include "league_context_routes.h"
#include "league_routes.h"
#include "lineup_game_lock.h"
#include "live_stat_routes.h"
#include "operations_routes.h"
#include "public_routes.h"

namespace cff::app_composition {

void registerApplicationRoutes(
    drogon::HttpAppFramework &app,
    const std::optional<std::string> &jwtSecret,
    const std::unordered_set<std::string> &allowedOrigins) {
    cff::health::registerHealthRoutes(app, jwtSecret, allowedOrigins);
    cff::auth::registerAuthRoutes(app, jwtSecret, allowedOrigins);
    cff::operations::registerOperationsRoutes(app, jwtSecret, allowedOrigins);
    cff::live_stats::registerLiveStatRoutes(app, jwtSecret, allowedOrigins);
    cff::league::registerLeagueRoutes(app, jwtSecret, allowedOrigins);
    cff::league_context::registerLeagueContextRoutes(app, jwtSecret, allowedOrigins);
    cff::lineup_game_lock::registerRoutes(app);
    cff::public_api::registerPublicRoutes(app, allowedOrigins);
}

} // namespace cff::app_composition
