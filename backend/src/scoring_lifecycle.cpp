#include "scoring_lifecycle.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace cff::scoring_lifecycle {
namespace {

std::string trim(std::string value) {
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), value.end());
    return value;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string stringValue(const Json::Value &value,
                        const char *key,
                        const std::string &fallback = "") {
    const auto &node = value[key];
    return node.isString() ? node.asString() : fallback;
}

int intValue(const Json::Value &value, const char *key, int fallback = 0) {
    const auto &node = value[key];
    return node.isInt() || node.isUInt() ? node.asInt() : fallback;
}

double numberValue(const Json::Value &value, const char *key, double fallback) {
    const auto &node = value[key];
    return node.isNumeric() ? node.asDouble() : fallback;
}

bool tokenMatches(const std::string &value,
                  std::initializer_list<const char *> options) {
    const auto token = canonicalStatToken(value);
    for (const auto *option : options) {
        if (token == option) return true;
    }
    return false;
}

struct StandingRow {
    std::string email;
    std::string teamName;
    std::string role;
    std::string status;
    int wins{0};
    int losses{0};
    int ties{0};
    int gamesPlayed{0};
    double pointsFor{0.0};
    double pointsAgainst{0.0};

    double winPct() const {
        return gamesPlayed > 0
            ? (static_cast<double>(wins) + static_cast<double>(ties) * 0.5)
                / static_cast<double>(gamesPlayed)
            : 0.0;
    }
};

} // namespace

std::string canonicalEmail(std::string value) {
    return lower(trim(std::move(value)));
}

std::string canonicalStatToken(std::string value) {
    value = lower(trim(std::move(value)));
    value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char ch) {
        return !std::isalnum(ch);
    }), value.end());
    return value;
}

long long normalizedVersion(const Json::Value &value, long long fallback) {
    if (value.isInt64() || value.isUInt64() || value.isInt() || value.isUInt()) {
        return value.asInt64();
    }
    if (value.isString()) {
        try {
            return std::stoll(value.asString());
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

bool expectedVersionMatches(long long currentVersion,
                            const Json::Value &body,
                            bool required) {
    if (!body.isObject() || !body.isMember("expectedVersion")) return !required;
    return normalizedVersion(body["expectedVersion"], -1) == currentVersion;
}

bool finalizedStatus(const std::string &status) {
    return canonicalStatToken(status) == "final";
}

bool scoredStatus(const std::string &status) {
    const auto normalized = canonicalStatToken(status);
    return normalized == "scored" || normalized == "final";
}

double fantasyPointsForStat(const Json::Value &settings,
                            const std::string &category,
                            const std::string &statName,
                            double value) {
    const auto cat = canonicalStatToken(category);
    if (cat == "passing") {
        if (tokenMatches(statName, {"passyards", "passingyards", "yds"})) {
            const auto divisor = numberValue(settings, "passingYardsPerPoint", 25.0);
            return divisor == 0.0 ? 0.0 : value / divisor;
        }
        if (tokenMatches(statName, {"passtd", "passingtd", "passingtouchdown", "touchdowns"})) {
            return value * numberValue(settings, "passingTd", 4.0);
        }
        if (tokenMatches(statName, {"interception", "interceptions", "int"})) {
            return value * numberValue(settings, "interception", -2.0);
        }
    }
    if (cat == "rushing") {
        if (tokenMatches(statName, {"rushyards", "rushingyards", "yds"})) {
            const auto divisor = numberValue(settings, "rushingYardsPerPoint", 10.0);
            return divisor == 0.0 ? 0.0 : value / divisor;
        }
        if (tokenMatches(statName, {"rushtd", "rushingtd", "rushingtouchdown", "touchdowns"})) {
            return value * numberValue(settings, "rushingTd", 6.0);
        }
    }
    if (cat == "receiving") {
        if (tokenMatches(statName, {"recyards", "receivingyards", "yds"})) {
            const auto divisor = numberValue(settings, "receivingYardsPerPoint", 10.0);
            return divisor == 0.0 ? 0.0 : value / divisor;
        }
        if (tokenMatches(statName, {"rectd", "receivingtd", "receivingtouchdown", "touchdowns"})) {
            return value * numberValue(settings, "receivingTd", 6.0);
        }
        if (tokenMatches(statName, {"reception", "receptions", "rec", "catches"})) {
            return value * numberValue(settings, "reception", 1.0);
        }
    }
    if (tokenMatches(statName, {"fumblelost", "fumbleslost"})) {
        return value * numberValue(settings, "fumbleLost", -2.0);
    }
    if (tokenMatches(statName, {"twopoint", "twopointconversion", "twopt"})) {
        return value * numberValue(settings, "twoPointConversion", 2.0);
    }
    return 0.0;
}

Json::Value standingsFromFinalMatchups(const Json::Value &members,
                                       const Json::Value &matchups,
                                       int season) {
    std::vector<StandingRow> rows;
    std::unordered_map<std::string, std::size_t> indexByEmail;
    if (members.isArray()) {
        for (const auto &member : members) {
            const auto email = canonicalEmail(stringValue(member, "email"));
            const auto status = lower(stringValue(member, "status", "active"));
            if (email.empty() || status == "removed" || status == "invited" || status == "pending") continue;
            if (indexByEmail.count(email)) continue;
            StandingRow row;
            row.email = email;
            row.teamName = stringValue(member, "teamName");
            row.role = stringValue(member, "role", "member");
            row.status = stringValue(member, "status", "Active");
            indexByEmail[email] = rows.size();
            rows.push_back(row);
        }
    }

    if (matchups.isArray()) {
        for (const auto &matchup : matchups) {
            if (!finalizedStatus(stringValue(matchup, "status", "scheduled"))) continue;
            if (season > 0 && matchup.isMember("season") && intValue(matchup, "season", season) != season) continue;
            const auto homeEmail = canonicalEmail(stringValue(matchup, "homeManager"));
            const auto awayEmail = canonicalEmail(stringValue(matchup, "awayManager"));
            if (homeEmail.empty() || awayEmail.empty()) continue;
            const auto homeIt = indexByEmail.find(homeEmail);
            const auto awayIt = indexByEmail.find(awayEmail);
            if (homeIt == indexByEmail.end() || awayIt == indexByEmail.end()) continue;
            auto &home = rows[homeIt->second];
            auto &away = rows[awayIt->second];
            const auto homeScore = numberValue(matchup, "homeScore", 0.0);
            const auto awayScore = numberValue(matchup, "awayScore", 0.0);
            home.pointsFor += homeScore;
            home.pointsAgainst += awayScore;
            away.pointsFor += awayScore;
            away.pointsAgainst += homeScore;
            ++home.gamesPlayed;
            ++away.gamesPlayed;
            if (std::fabs(homeScore - awayScore) < 0.000001) {
                ++home.ties;
                ++away.ties;
            } else if (homeScore > awayScore) {
                ++home.wins;
                ++away.losses;
            } else {
                ++away.wins;
                ++home.losses;
            }
        }
    }

    std::sort(rows.begin(), rows.end(), [](const StandingRow &left, const StandingRow &right) {
        if (std::fabs(left.winPct() - right.winPct()) > 0.000001) return left.winPct() > right.winPct();
        if (left.wins != right.wins) return left.wins > right.wins;
        if (left.losses != right.losses) return left.losses < right.losses;
        if (std::fabs(left.pointsFor - right.pointsFor) > 0.000001) return left.pointsFor > right.pointsFor;
        if (std::fabs(left.pointsAgainst - right.pointsAgainst) > 0.000001) return left.pointsAgainst < right.pointsAgainst;
        return left.email < right.email;
    });

    Json::Value result(Json::arrayValue);
    int rank = 1;
    for (const auto &row : rows) {
        Json::Value item(Json::objectValue);
        item["rank"] = rank++;
        item["email"] = row.email;
        item["managerEmail"] = row.email;
        item["teamName"] = row.teamName;
        item["role"] = row.role;
        item["status"] = row.status;
        item["wins"] = row.wins;
        item["losses"] = row.losses;
        item["ties"] = row.ties;
        item["gamesPlayed"] = row.gamesPlayed;
        item["pointsFor"] = row.pointsFor;
        item["pointsAgainst"] = row.pointsAgainst;
        item["winPct"] = row.winPct();
        result.append(item);
    }
    return result;
}

} // namespace cff::scoring_lifecycle
