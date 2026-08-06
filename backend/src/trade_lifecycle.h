#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <json/json.h>

namespace cff::trade_lifecycle {

struct TransitionDecision {
    bool allowed{false};
    bool execute{false};
    bool releaseLocks{false};
    std::string nextStatus;
    std::string errorCode;
};

std::string canonicalEmail(std::string value);
std::string canonicalPlayerId(std::string value);

bool openStatus(const std::string &status);
bool terminalStatus(const std::string &status);

bool expectedVersionMatches(long long currentVersion,
                            const Json::Value &body,
                            bool required = true);

TransitionDecision decideTransition(const std::string &currentStatus,
                                    const std::string &requestedStatus,
                                    const std::string &actorEmail,
                                    const std::string &offeredByEmail,
                                    const std::string &offeredToEmail,
                                    bool actorCommissioner,
                                    bool requiresApproval);

bool validOfferPlayers(const std::string &offeredPlayerId,
                       const std::string &requestedPlayerId);

bool validOfferPlayerPackages(const std::vector<std::string> &offeredPlayerIds,
                              const std::vector<std::string> &requestedPlayerIds,
                              std::size_t maximumPerSide = 20);

} // namespace cff::trade_lifecycle
