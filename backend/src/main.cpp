#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef DROGON_FOUND
#include <crypt.h>
#include <cpr/cpr.h>
#include <drogon/drogon.h>
#include <json/json.h>
#ifdef CFF_HAS_POSTGRES
#include <postgresql/libpq-fe.h>
#endif
#include "auth_core.h"
#include "auth_controller.h"
#include "auth_routes.h"
#include "http_security.h"
#include "health_routes.h"
#include "operations_routes.h"
#include "auth_account_store.h"
#include "auth_session_store.h"
#include "app_config.h"
#include "cfbd_ingest.h"
#include "email_delivery.h"
#include "live_scores.h"
#include "league_models.h"
#include "handlers/league_handler.h"
#include "player_catalog.h"
#endif

#ifdef DROGON_FOUND
namespace {
using cff::auth::canonicalEmail;
using cff::auth::hashPassword;
using cff::auth::randomToken;
using cff::auth::verifyPassword;
using cff::config::csvEmailSetFromEnv;
using cff::config::exposeAuthTokens;
using cff::config::logAuthTokens;
using cff::config::readEnv;
using cff::config::sharedSecretAuthAllowed;

void logIngestResult(const std::string &label, const cff::IngestResult &ingestResult) {
    std::cout << "[cfbd] " << label << " complete. inserted=" << ingestResult.ingested
              << " updated=" << ingestResult.updated
              << " api_calls=" << ingestResult.apiCalls << std::endl;
    for (const auto &err : ingestResult.errors) {
        std::cerr << "[cfbd] " << label << " error: " << err << std::endl;
    }
}

void startBackgroundCfbdIngest(int intervalHours) {
    std::thread([intervalHours]() {
        std::cout << "[cfbd] background ingest enabled every " << intervalHours << " hour(s)." << std::endl;
        while (true) {
            std::this_thread::sleep_for(std::chrono::hours(intervalHours));
            std::cout << "[cfbd] background ingest starting..." << std::endl;
            logIngestResult("background ingest", cff::runCfbdIngestOnce());
        }
    }).detach();
}

std::string jsonToString(const Json::Value &value) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, value);
}

#endif

std::string firstHeaderValue(std::string value) {
    const auto comma = value.find(',');
    if (comma != std::string::npos) {
        value = value.substr(0, comma);
    }
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), value.end());
    return value;
}

std::optional<std::string> getOptionalParam(const drogon::HttpRequestPtr &req, const std::string &key) {
    auto value = req->getParameter(key);
    if (value.empty()) {
        return std::nullopt;
    }
    return value;
}


} // namespace
#endif

int main(int argc, char* argv[]) {
#ifdef DROGON_FOUND
    auto &app = drogon::app();

    // Load environment configuration once at startup. Policy helpers used by
    // request handlers remain centralized in app_config.cpp.
    const auto runtimeConfig = cff::config::loadRuntimeConfig();
    const auto &port = runtimeConfig.port;
    const auto &jwtSecret = runtimeConfig.jwtSecret;
    const auto &sslCert = runtimeConfig.sslCert;
    const auto &sslKey = runtimeConfig.sslKey;
    const auto &ingestIntervalHours = runtimeConfig.ingestIntervalHours;

    if (!jwtSecret.has_value()) {
        std::cerr << "[security] JWT_SECRET is not set; secure endpoints will reject all requests." << std::endl;
    }

    // SSL enablement when certs are available
    const bool useSsl = runtimeConfig.sslEnabled();
    if (sslCert && sslKey) {
        app.setSSLFiles(sslCert.value(), sslKey.value());
        std::cout << "[security] SSL enabled with provided certificate and key." << std::endl;
    } else {
        std::cout << "[security] SSL not configured. For testing only. Provide SSL_CERT_FILE and SSL_KEY_FILE to enable HTTPS." << std::endl;
    }

    // Minimal CORS handling via post-routing advice
    const auto &allowedOrigins = runtimeConfig.allowedOrigins;
    if (!runtimeConfig.allowedOriginsConfigured) {
        std::cout << "[security] ALLOWED_ORIGINS not set; cross-origin requests will be blocked." << std::endl;
    }

    app.registerPostHandlingAdvice([allowedOrigins](const drogon::HttpRequestPtr &req,
                                                    const drogon::HttpResponsePtr &resp) {
        cff::http::applyCorsHeaders(req, resp, allowedOrigins);
    });

    auto preflightHandler = [allowedOrigins](const drogon::HttpRequestPtr &req,
                                             std::function<void (const drogon::HttpResponsePtr &)> &&callback) {
        callback(cff::http::buildPreflightResponse(req, allowedOrigins));
    };
    auto preflightOneParamHandler = [allowedOrigins](const drogon::HttpRequestPtr &req,
                                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                                     const std::string&) {
        callback(cff::http::buildPreflightResponse(req, allowedOrigins));
    };
    auto preflightTwoParamHandler = [allowedOrigins](const drogon::HttpRequestPtr &req,
                                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                                     const std::string&,
                                                     const std::string&) {
        callback(cff::http::buildPreflightResponse(req, allowedOrigins));
    };

    const bool ingestOnStartup = runtimeConfig.ingestOnStartup;
    if (ingestOnStartup) {
        std::cout << "[cfbd] CFBD_INGEST_ON_STARTUP enabled; starting ingest..." << std::endl;
        logIngestResult("startup ingest", cff::runCfbdIngestOnce());
    }

    if (ingestIntervalHours) {
        startBackgroundCfbdIngest(*ingestIntervalHours);
    }

    app.addListener("0.0.0.0", static_cast<unsigned short>(std::stoi(port)), useSsl)
        .setThreadNum(std::thread::hardware_concurrency());

    cff::health::registerHealthRoutes(app, jwtSecret, allowedOrigins);

    cff::auth::registerAuthRoutes(app, jwtSecret, allowedOrigins);

    cff::operations::registerOperationsRoutes(app, jwtSecret, allowedOrigins);

    app.registerHandler("/api/leagues",
                         [jwtSecret](const drogon::HttpRequestPtr& req, std::function<void (const drogon::HttpResponsePtr &)> &&callback) {
                             std::string accountEmail;
                             if (!cff::http::requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleListLeagues(req, std::move(callback), accountEmail);
                         },
                         {drogon::Get})
        .registerHandler("/api/leagues",
                         [jwtSecret](const drogon::HttpRequestPtr& req, std::function<void (const drogon::HttpResponsePtr &)> &&callback) {
                             std::string accountEmail;
                             if (!cff::http::requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleCreateLeague(req, std::move(callback), accountEmail);
                         },
                         {drogon::Post})
        .registerHandler("/api/leagues/{1}",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!cff::http::requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleGetLeague(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Get})
        .registerHandler("/api/leagues/{1}",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!cff::http::requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleUpdateLeague(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Put})
        .registerHandler("/api/leagues/{1}",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!cff::http::requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleDeleteLeague(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Delete})
        .registerHandler("/api/leagues/{1}/members",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!cff::http::requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleListMembers(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Get})
        .registerHandler("/api/leagues/{1}/members",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!cff::http::requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleInviteMember(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Post})
        .registerHandler("/api/leagues/{1}/members/{2}",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId,
                                     const std::string &memberEmail) {
                             std::string accountEmail;
                             if (!cff::http::requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleUpdateMember(req, std::move(callback), accountEmail, leagueId, memberEmail);
                         },
                         {drogon::Put, drogon::Post})
        .registerHandler("/api/leagues/{1}/join",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!cff::http::requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleJoinLeague(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Post})
        .registerHandler("/api/leagues/{1}/roster",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!cff::http::requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleGetRoster(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Get})
        .registerHandler("/api/leagues/{1}/rosters/{2}",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId,
                                     const std::string &managerEmail) {
                             std::string accountEmail;
                             if (!cff::http::requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleGetManagerRoster(req, std::move(callback), accountEmail, leagueId, managerEmail);
                         },
                         {drogon::Get})
        .registerHandler("/api/leagues/{1}/roster",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!cff::http::requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleAddRosterPlayer(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Post})
        .registerHandler("/api/leagues/{1}/roster/drop",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!cff::http::requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleDropRosterPlayer(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Post})
        .registerHandler("/api/leagues/{1}/roster/{2}/slot",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId,
                                     const std::string &playerId) {
                             std::string accountEmail;
                             if (!cff::http::requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleUpdateRosterSlot(req, std::move(callback), accountEmail, leagueId, playerId);
                         },
                         {drogon::Post, drogon::Put})
        .registerHandler("/api/leagues/{1}/free-agents",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!cff::http::requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleFreeAgents(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Get})
        .registerHandler("/api/leagues/{1}/draft",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!cff::http::requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleGetDraftState(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Get})
        .registerHandler("/api/leagues/{1}/draft/queue",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!cff::http::requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleSaveDraftQueue(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Put, drogon::Post})
        .registerHandler("/api/leagues/{1}/draft/order",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!cff::http::requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleSaveDraftOrder(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Put, drogon::Post})
        .registerHandler("/api/leagues/{1}/draft/picks",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!cff::http::requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleMakeDraftPick(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Post})
        .registerHandler("/api/leagues/{1}/draft/reset",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!cff::http::requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleResetDraft(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Post})
        .registerHandler("/api/leagues/{1}/draft/undo",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!cff::http::requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleUndoDraftPick(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Post})
        .registerHandler("/api/leagues/{1}/waivers",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!cff::http::requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleListWaivers(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Get})
        .registerHandler("/api/leagues/{1}/waivers",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!cff::http::requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleCreateWaiver(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Post})
        .registerHandler("/api/leagues/{1}/waivers/process",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!cff::http::requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleProcessWaivers(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Post})
        .registerHandler("/api/leagues/{1}/waivers/{2}/process",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId,
                                     const std::string &claimId) {
                             std::string accountEmail;
                             if (!cff::http::requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleProcessWaiver(req, std::move(callback), accountEmail, leagueId, claimId);
                         },
                         {drogon::Post})
        .registerHandler("/api/leagues/{1}/waivers/{2}/status",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId,
                                     const std::string &claimId) {
                             std::string accountEmail;
                             if (!cff::http::requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleUpdateWaiverStatus(req, std::move(callback), accountEmail, leagueId, claimId);
                         },
                         {drogon::Post})
        .registerHandler("/api/leagues/{1}/waivers/reorder",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!cff::http::requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleReorderWaivers(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Post})
        .registerHandler("/api/leagues/{1}/waiver-priority",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!cff::http::requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleListWaiverPriority(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Get})
        .registerHandler("/api/leagues/{1}/waiver-priority/reset",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!cff::http::requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleResetWaiverPriority(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Post})
        .registerHandler("/api/leagues/{1}/trades",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!cff::http::requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleListTrades(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Get})
        .registerHandler("/api/leagues/{1}/trades",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!cff::http::requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleCreateTrade(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Post})
        .registerHandler("/api/leagues/{1}/trades/{2}/status",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId,
                                     const std::string &tradeId) {
                             std::string accountEmail;
                             if (!cff::http::requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleUpdateTradeStatus(req, std::move(callback), accountEmail, leagueId, tradeId);
                         },
                         {drogon::Post})
        .registerHandler("/api/leagues/{1}/matchups",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!cff::http::requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleListMatchups(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Get})
        .registerHandler("/api/leagues/{1}/matchups/generate",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!cff::http::requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleGenerateMatchups(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Post})
        .registerHandler("/api/leagues/{1}/matchups/generate-season",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!cff::http::requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleGenerateSeasonSchedule(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Post})
        .registerHandler("/api/leagues/{1}/score/week/{2}",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId,
                                     const std::string &week) {
                             std::string accountEmail;
                             if (!cff::http::requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleScoreWeek(req, std::move(callback), accountEmail, leagueId, week);
                         },
                         {drogon::Post})
        .registerHandler("/api/leagues/{1}/score/week/{2}/finalize",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId,
                                     const std::string &week) {
                             std::string accountEmail;
                             if (!cff::http::requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleFinalizeWeek(req, std::move(callback), accountEmail, leagueId, week);
                         },
                         {drogon::Post})
        .registerHandler("/api/leagues/{1}/transactions",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!cff::http::requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleListTransactions(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Get})
        .registerHandler("/api/leagues/{1}/feed",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!cff::http::requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleListLeagueFeed(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Get})
        .registerHandler("/api/leagues/{1}/feed/posts",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!cff::http::requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleCreateLeagueFeedPost(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Post})
        .registerHandler("/api/scores/live",
                         [](const drogon::HttpRequestPtr&, std::function<void (const drogon::HttpResponsePtr &)> &&callback) {
                             auto resp = drogon::HttpResponse::newHttpJsonResponse(cff::cachedLiveScorePayload());
                             resp->setStatusCode(drogon::k200OK);
                             callback(resp);
                         },
                         {drogon::Get})
        .registerHandler("/api/scores/live/meta",
                         [](const drogon::HttpRequestPtr&, std::function<void (const drogon::HttpResponsePtr &)> &&callback) {
                             auto resp = drogon::HttpResponse::newHttpJsonResponse(cff::cachedLiveScoreMeta());
                             resp->setStatusCode(drogon::k200OK);
                             callback(resp);
                         },
                         {drogon::Get})
        .registerHandler("/api/players/meta",
                         [](const drogon::HttpRequestPtr&, std::function<void (const drogon::HttpResponsePtr &)> &&callback) {
                             auto resp = drogon::HttpResponse::newHttpJsonResponse(cff::playerCatalogMeta());
                             resp->setStatusCode(drogon::k200OK);
                             callback(resp);
                         },
                         {drogon::Get})
        .registerHandler("/api/players",
                         [](const drogon::HttpRequestPtr& req, std::function<void (const drogon::HttpResponsePtr &)> &&callback) {
#ifndef CFF_HAS_POSTGRES
                             Json::Value error;
                             error["error"] = "Player search unavailable: backend not built with PostgreSQL support.";
                             auto resp = drogon::HttpResponse::newHttpJsonResponse(error);
                             resp->setStatusCode(drogon::k503ServiceUnavailable);
                             callback(resp);
                             return;
#else
                             const auto query = req->getParameter("query");
                             auto positionFilter = getOptionalParam(req, "position");
                             auto conferenceFilter = getOptionalParam(req, "conference");
                             auto teamFilter = getOptionalParam(req, "team");

                             std::size_t limit = 25;
                             const auto limitParam = req->getParameter("limit");
                             if (!limitParam.empty()) {
                                 char *end = nullptr;
                                 const auto parsed = std::strtoul(limitParam.c_str(), &end, 10);
                                 if (end != limitParam.c_str() && parsed > 0) {
                                     limit = std::min<std::size_t>(parsed, 100);
                                 }
                             }

                             std::size_t offset = 0;
                             const auto offsetParam = req->getParameter("offset");
                             if (!offsetParam.empty()) {
                                 char *end = nullptr;
                                 const auto parsed = std::strtoul(offsetParam.c_str(), &end, 10);
                                 if (end != offsetParam.c_str()) offset = std::min<std::size_t>(parsed, 5000);
                             }

                             const auto results = cff::searchPlayers(
                                 query, positionFilter, conferenceFilter, teamFilter, limit, offset
                             );
                             Json::Value payload(Json::arrayValue);
                             for (const auto &player : results) {
                                 payload.append(player.toJson());
                             }

                             auto resp = drogon::HttpResponse::newHttpJsonResponse(payload);
                             resp->setStatusCode(drogon::k200OK);
                             callback(resp);
 #endif
                         },
                         {drogon::Get})
        .registerHandler("/api/leagues", preflightHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}", preflightOneParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/members", preflightOneParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/members/{2}", preflightTwoParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/join", preflightOneParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/roster", preflightOneParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/rosters/{2}", preflightTwoParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/roster/drop", preflightOneParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/roster/{2}/slot", preflightTwoParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/free-agents", preflightOneParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/draft", preflightOneParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/draft/queue", preflightOneParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/draft/order", preflightOneParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/draft/picks", preflightOneParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/draft/reset", preflightOneParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/draft/undo", preflightOneParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/waivers", preflightOneParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/waivers/process", preflightOneParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/waivers/{2}/process", preflightTwoParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/waivers/{2}/status", preflightTwoParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/waivers/reorder", preflightOneParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/waiver-priority", preflightOneParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/waiver-priority/reset", preflightOneParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/trades", preflightOneParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/trades/{2}/status", preflightTwoParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/matchups", preflightOneParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/matchups/generate", preflightOneParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/matchups/generate-season", preflightOneParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/score/week/{2}", preflightTwoParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/score/week/{2}/finalize", preflightTwoParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/transactions", preflightOneParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/feed", preflightOneParamHandler, {drogon::Options})
        .registerHandler("/api/leagues/{1}/feed/posts", preflightOneParamHandler, {drogon::Options})
        .registerHandler("/api/scores/live", preflightHandler, {drogon::Options})
        .registerHandler("/api/scores/live/meta", preflightHandler, {drogon::Options})
        .registerHandler("/api/players", preflightHandler, {drogon::Options})
        .registerHandler("/api/players/meta", preflightHandler, {drogon::Options})
        .run();
#else
    // Stub output to avoid hard dependency on Drogon in early scaffolding.
    std::cout << "College Fantasy Football backend scaffold (Drogon not linked)." << std::endl;
    std::cout << "Build with -DDROGON_FOUND=ON and link Drogon to run the HTTP server." << std::endl;
#endif
    return 0;
}
