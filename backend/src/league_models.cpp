#include "league_models.h"

#include <chrono>
#include <random>
#include <string>
#include <string_view>

namespace {
int clampTeams(int teams) {
    if (teams < 4) {
        return 4;
    }
    if (teams > 16) {
        return 16;
    }
    return teams;
}
} // namespace

namespace cff {

TeamSettings TeamSettings::fromJson(const Json::Value &body) {
    TeamSettings settings;
    if (body.isInt()) {
        settings.teamCount = clampTeams(body.asInt());
        return settings;
    }
    if (body.isObject() && body.isMember("teams") && body["teams"].isInt()) {
        settings.teamCount = clampTeams(body["teams"].asInt());
    }
    return settings;
}

Json::Value TeamSettings::toJson() const {
    Json::Value json;
    json["teams"] = teamCount;
    return json;
}

ScoringSettings ScoringSettings::fromId(std::string_view scoringId) {
    ScoringSettings scoring;
    if (scoringId == "half_ppr") {
        scoring.id = "half_ppr";
        scoring.label = "Half-PPR";
    } else if (scoringId == "standard") {
        scoring.id = "standard";
        scoring.label = "Standard";
    } else {
        scoring.id = "ppr";
        scoring.label = "PPR";
    }
    return scoring;
}

Json::Value ScoringSettings::toJson() const {
    Json::Value json;
    json["scoring"] = id;
    json["scoringLabel"] = label;
    return json;
}

DraftSettings DraftSettings::fromId(std::string_view draftId) {
    DraftSettings draft;
    if (draftId == "auction") {
        draft.type = "auction";
        draft.label = "Auction";
    } else {
        draft.type = "snake";
        draft.label = "Snake";
    }
    return draft;
}

Json::Value DraftSettings::toJson() const {
    Json::Value json;
    json["draftType"] = type;
    json["draftTypeLabel"] = label;
    return json;
}

League League::fromJson(const Json::Value &body) {
    League league;
    if (body.isMember("name") && body["name"].isString()) {
        league.name = body["name"].asString();
    }
    if (body.isMember("teams")) {
        league.teams = TeamSettings::fromJson(body["teams"]);
    }
    if (body.isMember("scoring") && body["scoring"].isString()) {
        league.scoring = ScoringSettings::fromId(body["scoring"].asString());
    }
    if (body.isMember("draftType") && body["draftType"].isString()) {
        league.draft = DraftSettings::fromId(body["draftType"].asString());
    }
    if (body.isMember("notes") && body["notes"].isString()) {
        league.notes = body["notes"].asString();
    }
    league.id = generateLeagueId();
    return league;
}

Json::Value League::toJson() const {
    Json::Value json;
    json["id"] = id;
    json["name"] = name;
    json["teams"] = teams.teamCount;
    json["scoring"] = scoring.id;
    json["scoringLabel"] = scoring.label;
    json["draftType"] = draft.type;
    json["draftTypeLabel"] = draft.label;
    json["notes"] = notes;
    return json;
}

std::string generateLeagueId() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::random_device rd;
    std::mt19937_64 gen(rd() ^ static_cast<std::mt19937_64::result_type>(now));
    std::uniform_int_distribution<std::uint64_t> dist;
    auto token = dist(gen);
    return "league-" + std::to_string(token);
}

} // namespace cff
