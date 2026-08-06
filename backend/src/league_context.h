#pragma once

#include <json/json.h>

#include <algorithm>
#include <cctype>
#include <string>

namespace cff::league_context {
namespace detail {

inline std::string canonicalText(std::string value) {
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), value.end());
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

inline const Json::Value *findActiveMember(const Json::Value &members,
                                           const std::string &accountEmail) {
    if (!members.isArray()) {
        return nullptr;
    }
    const auto canonicalAccountEmail = canonicalText(accountEmail);
    for (const auto &member : members) {
        if (!member.isObject()) {
            continue;
        }
        const auto memberEmail = canonicalText(member.get("email", "").asString());
        const auto memberStatus = canonicalText(member.get("status", "").asString());
        if (memberEmail == canonicalAccountEmail && memberStatus == "active") {
            return &member;
        }
    }
    return nullptr;
}

}  // namespace detail

inline Json::Value buildLeagueContext(const Json::Value &league,
                                      const std::string &accountEmail,
                                      bool commissionerFallback,
                                      const std::string &serverTime) {
    const auto *member = detail::findActiveMember(league["members"], accountEmail);
    const auto memberRole = member == nullptr
        ? std::string{}
        : detail::canonicalText(member->get("role", "member").asString());
    const bool commissioner = commissionerFallback || memberRole == "commissioner";
    const auto teamName = member == nullptr
        ? std::string{}
        : member->get("teamName", member->get("team_name", "")).asString();
    const bool teamAssigned = !detail::canonicalText(teamName).empty();

    Json::Value permissions(Json::objectValue);
    permissions["canEditLineup"] = teamAssigned;
    permissions["canAddPlayers"] = teamAssigned;
    permissions["canProposeTrades"] = teamAssigned;
    permissions["canManageLeague"] = commissioner;

    Json::Value context(Json::objectValue);
    context["leagueId"] = league.get("id", "").asString();
    context["leagueName"] = league.get("name", "").asString();
    context["userRole"] = commissioner ? "COMMISSIONER" : "MEMBER";
    context["isCommissioner"] = commissioner;
    context["teamAssigned"] = teamAssigned;
    if (teamAssigned) {
        // Existing roster APIs use manager email as the team ownership key.
        context["teamId"] = accountEmail;
        context["teamName"] = teamName;
    } else {
        context["teamId"] = Json::nullValue;
        context["teamName"] = "";
    }
    context["permissions"] = permissions;
    context["serverTime"] = serverTime;
    return context;
}

}  // namespace cff::league_context
