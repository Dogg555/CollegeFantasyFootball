#pragma once

#include <json/json.h>

#include <string>

namespace cff::draft_lifecycle {

std::string canonicalEmail(std::string value);

Json::Value canonicalOrder(const Json::Value &managerEmails);

bool orderMatchesManagers(const Json::Value &draftOrder,
                          const Json::Value &managerEmails);

int rosterSlotsPerManager(const Json::Value &rosterRules);

int totalDraftPicks(int managerCount, const Json::Value &rosterRules);

bool draftCompleteAfterPick(int completedPickNumber,
                            int managerCount,
                            const Json::Value &rosterRules);

std::string managerForPick(const Json::Value &draftOrder,
                           int pickNumber,
                           const std::string &draftType = "snake");

bool allManagersReady(const Json::Value &managerEmails,
                      const Json::Value &readiness);

} // namespace cff::draft_lifecycle
