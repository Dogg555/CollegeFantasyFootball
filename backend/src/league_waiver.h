#pragma once

#include <string>
#include <vector>

#include <json/json.h>

namespace cff::league_waiver {

bool modeActive(const Json::Value &rules);

bool deadlinePassedAt(const Json::Value &rules,
                      const std::string &currentIsoMinute);

bool deadlinePassed(const Json::Value &rules);

int nextClaimOrder(const Json::Value &claims,
                   const std::string &managerEmail);

std::vector<Json::ArrayIndex> orderedClaimIndexes(const Json::Value &claims);

Json::Value buildPriorityBoard(const Json::Value &members);

std::string claimTransactionSummary(const Json::Value &player);
std::string processedTransactionSummary(const Json::Value &player);
std::string cancelledTransactionSummary();
std::string resetPriorityTransactionSummary();

} // namespace cff::league_waiver
