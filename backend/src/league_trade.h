#pragma once

#include <string>

#include <json/json.h>

namespace cff::league_trade {

struct StatusDecision {
    bool allowed{false};
    bool execute{false};
    bool commissionerRequired{false};
    std::string databaseStatus;
    std::string displayStatus;
};

bool approvalRequired(const Json::Value &rules);
int expirationHours(const Json::Value &rules);

bool validTarget(const std::string &accountEmail,
                 const std::string &targetEmail);

bool requestStatusAllowed(const std::string &status);
bool potentiallyExecutes(const std::string &status);
bool openStatus(const std::string &status);

StatusDecision decideStatus(const std::string &requestedStatus,
                            bool requiresApproval,
                            bool actorInvolved,
                            bool actorCommissioner,
                            bool enforceParticipantRules);

bool playerLockedInOpenOffer(const Json::Value &offers,
                             const std::string &managerEmail,
                             const std::string &playerId);

std::string offerTransactionSummary(const Json::Value &player);
std::string statusTransactionSummary(const std::string &displayStatus,
                                     const Json::Value &player);

} // namespace cff::league_trade
