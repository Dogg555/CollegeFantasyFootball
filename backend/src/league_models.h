#pragma once

#include <json/json.h>
#include <string>
#include <string_view>
#include <vector>

namespace cff {

struct TeamSettings {
    int teamCount{10};

    static TeamSettings fromJson(const Json::Value &body);
    Json::Value toJson() const;
};

struct ScoringSettings {
    std::string id{"ppr"};
    std::string label{"PPR"};

    static ScoringSettings fromId(std::string_view scoringId);
    Json::Value toJson() const;
};

struct DraftSettings {
    std::string type{"snake"};
    std::string label{"Snake"};

    static DraftSettings fromId(std::string_view draftId);
    Json::Value toJson() const;
};

struct League {
    std::string id;
    std::string name{"New League"};
    TeamSettings teams{};
    ScoringSettings scoring{};
    DraftSettings draft{};
    std::string notes{};
    std::vector<std::string> invitedEmails{};

    static League fromJson(const Json::Value &body);
    Json::Value toJson() const;
};

std::string generateLeagueId();

} // namespace cff
