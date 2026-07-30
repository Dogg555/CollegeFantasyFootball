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
    double passingYardsPerPoint{25.0};
    double passingTd{4.0};
    double interception{-2.0};
    double rushingYardsPerPoint{10.0};
    double rushingTd{6.0};
    double receivingYardsPerPoint{10.0};
    double receivingTd{6.0};
    double reception{1.0};
    double fumbleLost{-2.0};
    double twoPointConversion{2.0};

    static ScoringSettings fromId(std::string_view scoringId);
    void applyJsonSettings(const Json::Value &body);
    Json::Value toJson() const;
};

struct DraftSettings {
    std::string type{"snake"};
    std::string label{"Snake"};

    static DraftSettings fromId(std::string_view draftId);
    Json::Value toJson() const;
};

struct RosterRules {
    int qb{1};
    int rb{2};
    int wr{2};
    int te{1};
    int flex{2};
    int bench{6};

    static RosterRules fromJson(const Json::Value &body);
    Json::Value toJson() const;
};

struct League {
    std::string id;
    std::string name{"New League"};
    TeamSettings teams{};
    ScoringSettings scoring{};
    DraftSettings draft{};
    RosterRules rosterRules{};
    Json::Value waiverRules{Json::objectValue};
    Json::Value tradeRules{Json::objectValue};
    std::string draftDate{};
    bool draftLobbyOpen{false};
    std::string draftLobbyStartedAt{};
    std::string notes{};
    std::vector<std::string> invitedEmails{};

    static League fromJson(const Json::Value &body);
    Json::Value toJson() const;
};

std::string generateLeagueId();

} // namespace cff
