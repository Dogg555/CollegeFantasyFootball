#pragma once

#ifdef DROGON_FOUND
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <functional>

namespace cff::handlers {

void handleCreateLeague(const drogon::HttpRequestPtr &req,
                        std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                        const std::string &accountEmail);

void handleListLeagues(const drogon::HttpRequestPtr &req,
                       std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                       const std::string &accountEmail);

void handleGetLeague(const drogon::HttpRequestPtr &req,
                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                     const std::string &accountEmail,
                     const std::string &leagueId);

void handleUpdateLeague(const drogon::HttpRequestPtr &req,
                        std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                        const std::string &accountEmail,
                        const std::string &leagueId);

void handleDeleteLeague(const drogon::HttpRequestPtr &req,
                        std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                        const std::string &accountEmail,
                        const std::string &leagueId);

void handleListMembers(const drogon::HttpRequestPtr &req,
                       std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                       const std::string &accountEmail,
                       const std::string &leagueId);

void handleInviteMember(const drogon::HttpRequestPtr &req,
                        std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                        const std::string &accountEmail,
                        const std::string &leagueId);

void handleUpdateMember(const drogon::HttpRequestPtr &req,
                        std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                        const std::string &accountEmail,
                        const std::string &leagueId,
                        const std::string &memberEmail);

void handleJoinLeague(const drogon::HttpRequestPtr &req,
                      std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                      const std::string &accountEmail,
                      const std::string &leagueId);

void handleGetRoster(const drogon::HttpRequestPtr &req,
                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                     const std::string &accountEmail,
                     const std::string &leagueId);

void handleGetManagerRoster(const drogon::HttpRequestPtr &req,
                            std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                            const std::string &accountEmail,
                            const std::string &leagueId,
                            const std::string &managerEmail);

void handleAddRosterPlayer(const drogon::HttpRequestPtr &req,
                           std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                           const std::string &accountEmail,
                           const std::string &leagueId);

void handleDropRosterPlayer(const drogon::HttpRequestPtr &req,
                            std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                            const std::string &accountEmail,
                            const std::string &leagueId);

void handleUpdateRosterSlot(const drogon::HttpRequestPtr &req,
                            std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                            const std::string &accountEmail,
                            const std::string &leagueId,
                            const std::string &playerId);

void handleFreeAgents(const drogon::HttpRequestPtr &req,
                      std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                      const std::string &accountEmail,
                      const std::string &leagueId);

void handleGetDraftState(const drogon::HttpRequestPtr &req,
                         std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                         const std::string &accountEmail,
                         const std::string &leagueId);

void handleSaveDraftQueue(const drogon::HttpRequestPtr &req,
                          std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                          const std::string &accountEmail,
                          const std::string &leagueId);

void handleSaveDraftOrder(const drogon::HttpRequestPtr &req,
                          std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                          const std::string &accountEmail,
                          const std::string &leagueId);

void handleMakeDraftPick(const drogon::HttpRequestPtr &req,
                         std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                         const std::string &accountEmail,
                         const std::string &leagueId);

void handleResetDraft(const drogon::HttpRequestPtr &req,
                      std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                      const std::string &accountEmail,
                      const std::string &leagueId);

void handleUndoDraftPick(const drogon::HttpRequestPtr &req,
                         std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                         const std::string &accountEmail,
                         const std::string &leagueId);

void handleListWaivers(const drogon::HttpRequestPtr &req,
                       std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                       const std::string &accountEmail,
                       const std::string &leagueId);

void handleCreateWaiver(const drogon::HttpRequestPtr &req,
                        std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                        const std::string &accountEmail,
                        const std::string &leagueId);

void handleProcessWaiver(const drogon::HttpRequestPtr &req,
                         std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                         const std::string &accountEmail,
                         const std::string &leagueId,
                         const std::string &claimId);

void handleUpdateWaiverStatus(const drogon::HttpRequestPtr &req,
                              std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                              const std::string &accountEmail,
                              const std::string &leagueId,
                              const std::string &claimId);

void handleReorderWaivers(const drogon::HttpRequestPtr &req,
                          std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                          const std::string &accountEmail,
                          const std::string &leagueId);

void handleProcessWaivers(const drogon::HttpRequestPtr &req,
                          std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                          const std::string &accountEmail,
                          const std::string &leagueId);

void handleListWaiverPriority(const drogon::HttpRequestPtr &req,
                              std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                              const std::string &accountEmail,
                              const std::string &leagueId);

void handleResetWaiverPriority(const drogon::HttpRequestPtr &req,
                               std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                               const std::string &accountEmail,
                               const std::string &leagueId);

void handleListTrades(const drogon::HttpRequestPtr &req,
                      std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                      const std::string &accountEmail,
                      const std::string &leagueId);

void handleCreateTrade(const drogon::HttpRequestPtr &req,
                       std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                       const std::string &accountEmail,
                       const std::string &leagueId);

void handleUpdateTradeStatus(const drogon::HttpRequestPtr &req,
                             std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                             const std::string &accountEmail,
                             const std::string &leagueId,
                             const std::string &tradeId);

void handleListMatchups(const drogon::HttpRequestPtr &req,
                        std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                        const std::string &accountEmail,
                        const std::string &leagueId);

void handleGenerateMatchups(const drogon::HttpRequestPtr &req,
                            std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                            const std::string &accountEmail,
                            const std::string &leagueId);

void handleGenerateSeasonSchedule(const drogon::HttpRequestPtr &req,
                                  std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                  const std::string &accountEmail,
                                  const std::string &leagueId);

void handleScoreWeek(const drogon::HttpRequestPtr &req,
                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                     const std::string &accountEmail,
                     const std::string &leagueId,
                     const std::string &week);

void handleFinalizeWeek(const drogon::HttpRequestPtr &req,
                        std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                        const std::string &accountEmail,
                        const std::string &leagueId,
                        const std::string &week);

void handleListTransactions(const drogon::HttpRequestPtr &req,
                            std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                            const std::string &accountEmail,
                            const std::string &leagueId);

void handleListLeagueFeed(const drogon::HttpRequestPtr &req,
                          std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                          const std::string &accountEmail,
                          const std::string &leagueId);

void handleCreateLeagueFeedPost(const drogon::HttpRequestPtr &req,
                                std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                const std::string &accountEmail,
                                const std::string &leagueId);

}
#endif // DROGON_FOUND
