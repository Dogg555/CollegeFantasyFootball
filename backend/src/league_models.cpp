#include "league_models.h"
#include "json_utils.h"

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

int clampRosterSlot(int value, int fallback) {
    if (value < 0) {
        return fallback;
    }
    if (value > 20) {
        return 20;
    }
    return value;
}

double getDoubleOrDefault(const Json::Value &json, const char *key, double fallback) {
    const auto &node = json[key];
    if (node.isNumeric()) {
        return node.asDouble();
    }
    return fallback;
}

double clampScoreValue(double value, double min, double max, double fallback) {
    if (value < min || value > max) {
        return fallback;
    }
    return value;
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
        scoring.reception = 0.5;
    } else if (scoringId == "standard") {
        scoring.id = "standard";
        scoring.label = "Standard";
        scoring.reception = 0.0;
    } else {
        scoring.id = "ppr";
        scoring.label = "PPR";
    }
    return scoring;
}

void ScoringSettings::applyJsonSettings(const Json::Value &body) {
    if (!body.isObject()) {
        return;
    }
    passingYardsPerPoint = clampScoreValue(getDoubleOrDefault(body, "passingYardsPerPoint", passingYardsPerPoint), 1.0, 100.0, passingYardsPerPoint);
    passingTd = clampScoreValue(getDoubleOrDefault(body, "passingTd", passingTd), -20.0, 20.0, passingTd);
    interception = clampScoreValue(getDoubleOrDefault(body, "interception", interception), -20.0, 20.0, interception);
    rushingYardsPerPoint = clampScoreValue(getDoubleOrDefault(body, "rushingYardsPerPoint", rushingYardsPerPoint), 1.0, 100.0, rushingYardsPerPoint);
    rushingTd = clampScoreValue(getDoubleOrDefault(body, "rushingTd", rushingTd), -20.0, 20.0, rushingTd);
    receivingYardsPerPoint = clampScoreValue(getDoubleOrDefault(body, "receivingYardsPerPoint", receivingYardsPerPoint), 1.0, 100.0, receivingYardsPerPoint);
    receivingTd = clampScoreValue(getDoubleOrDefault(body, "receivingTd", receivingTd), -20.0, 20.0, receivingTd);
    reception = clampScoreValue(getDoubleOrDefault(body, "reception", reception), 0.0, 5.0, reception);
    fumbleLost = clampScoreValue(getDoubleOrDefault(body, "fumbleLost", fumbleLost), -20.0, 20.0, fumbleLost);
    twoPointConversion = clampScoreValue(getDoubleOrDefault(body, "twoPointConversion", twoPointConversion), -20.0, 20.0, twoPointConversion);
}

Json::Value ScoringSettings::toJson() const {
    Json::Value json;
    json["scoring"] = id;
    json["scoringLabel"] = label;
    Json::Value settings;
    settings["passingYardsPerPoint"] = passingYardsPerPoint;
    settings["passingTd"] = passingTd;
    settings["interception"] = interception;
    settings["rushingYardsPerPoint"] = rushingYardsPerPoint;
    settings["rushingTd"] = rushingTd;
    settings["receivingYardsPerPoint"] = receivingYardsPerPoint;
    settings["receivingTd"] = receivingTd;
    settings["reception"] = reception;
    settings["fumbleLost"] = fumbleLost;
    settings["twoPointConversion"] = twoPointConversion;
    json["scoringSettings"] = settings;
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

RosterRules RosterRules::fromJson(const Json::Value &body) {
    RosterRules rules;
    if (!body.isObject()) {
        return rules;
    }
    rules.qb = clampRosterSlot(cff::getIntOrDefault(body, "qb", rules.qb), rules.qb);
    rules.rb = clampRosterSlot(cff::getIntOrDefault(body, "rb", rules.rb), rules.rb);
    rules.wr = clampRosterSlot(cff::getIntOrDefault(body, "wr", rules.wr), rules.wr);
    rules.te = clampRosterSlot(cff::getIntOrDefault(body, "te", rules.te), rules.te);
    rules.flex = clampRosterSlot(cff::getIntOrDefault(body, "flex", rules.flex), rules.flex);
    rules.bench = clampRosterSlot(cff::getIntOrDefault(body, "bench", rules.bench), rules.bench);
    return rules;
}

Json::Value RosterRules::toJson() const {
    Json::Value json;
    json["qb"] = qb;
    json["rb"] = rb;
    json["wr"] = wr;
    json["te"] = te;
    json["flex"] = flex;
    json["bench"] = bench;
    return json;
}

League League::fromJson(const Json::Value &body) {
    League league;
    league.name = cff::getStringOrDefault(body, "name", "New League");
    if (body.isMember("teams")) {
        league.teams = TeamSettings::fromJson(body["teams"]);
    }
    if (body.isMember("scoring") && body["scoring"].isString()) {
        league.scoring = ScoringSettings::fromId(body["scoring"].asString());
    }
    if (body.isMember("scoringSettings") && body["scoringSettings"].isObject()) {
        league.scoring.applyJsonSettings(body["scoringSettings"]);
    }
    if (body.isMember("draftType") && body["draftType"].isString()) {
        league.draft = DraftSettings::fromId(body["draftType"].asString());
    }
    if (body.isMember("rosterRules") && body["rosterRules"].isObject()) {
        league.rosterRules = RosterRules::fromJson(body["rosterRules"]);
    }
    if (body.isMember("waiverRules") && body["waiverRules"].isObject()) {
        league.waiverRules = body["waiverRules"];
    } else {
        league.waiverRules["mode"] = "free_agency";
        league.waiverRules["claimDeadline"] = "";
        league.waiverRules["freeAgencyLocked"] = false;
    }
    if (body.isMember("tradeRules") && body["tradeRules"].isObject()) {
        league.tradeRules = body["tradeRules"];
    } else {
        league.tradeRules["commissionerApproval"] = false;
        league.tradeRules["expirationHours"] = 48;
    }
    league.draftDate = cff::getStringOrDefault(body, "draftDate", "");
    league.draftLobbyOpen = body.isMember("draftLobbyOpen") && body["draftLobbyOpen"].isBool()
                                ? body["draftLobbyOpen"].asBool()
                                : false;
    league.draftLobbyStartedAt = cff::getStringOrDefault(body, "draftLobbyStartedAt", "");
    league.notes = cff::getStringOrDefault(body, "notes", "");
    if (body.isMember("invitedEmails") && body["invitedEmails"].isArray()) {
        for (const auto &email : body["invitedEmails"]) {
            if (email.isString()) {
                league.invitedEmails.push_back(email.asString());
            }
        }
    }
    league.id = generateLeagueId();
    return league;
}

Json::Value League::toJson() const {
    Json::Value json;
    const auto scoringJson = scoring.toJson();
    json["id"] = id;
    json["name"] = name;
    json["teams"] = teams.teamCount;
    json["scoring"] = scoringJson["scoring"];
    json["scoringLabel"] = scoringJson["scoringLabel"];
    json["scoringSettings"] = scoringJson["scoringSettings"];
    json["draftType"] = draft.type;
    json["draftTypeLabel"] = draft.label;
    json["draftDate"] = draftDate;
    json["draftLobbyOpen"] = draftLobbyOpen;
    json["draftLobbyStartedAt"] = draftLobbyStartedAt;
    json["rosterRules"] = rosterRules.toJson();
    json["waiverRules"] = waiverRules;
    json["tradeRules"] = tradeRules;
    json["notes"] = notes;
    Json::Value invites(Json::arrayValue);
    for (const auto &email : invitedEmails) {
        invites.append(email);
    }
    json["invitedEmails"] = invites;
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
