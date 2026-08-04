#include "league_trade.h"

#include <algorithm>
#include <cctype>

#include "json_utils.h"

namespace cff::league_trade {

namespace {

std::string lowerString(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string stringField(const Json::Value &value,
                        const std::string &key,
                        const std::string &fallback = "") {
    const auto &node = value[key];
    return node.isString() ? node.asString() : fallback;
}

std::string displayStatusFor(const std::string &databaseStatus) {
    if (databaseStatus == "accepted") return "Accepted";
    if (databaseStatus == "approved") return "Approved";
    if (databaseStatus == "vetoed") return "Vetoed";
    if (databaseStatus == "declined") return "Declined";
    if (databaseStatus == "cancelled") return "Cancelled";
    return "";
}

} // namespace

bool approvalRequired(const Json::Value &rules) {
    return rules.isMember("commissionerApproval")
        && rules["commissionerApproval"].asBool();
}

int expirationHours(const Json::Value &rules) {
    return std::clamp(cff::getIntOrDefault(rules, "expirationHours", 48), 1, 336);
}

bool validTarget(const std::string &accountEmail,
                 const std::string &targetEmail) {
    return !targetEmail.empty() && targetEmail != accountEmail;
}

bool requestStatusAllowed(const std::string &status) {
    return status == "Accepted"
        || status == "Approved"
        || status == "Vetoed"
        || status == "Declined"
        || status == "Cancelled";
}

bool potentiallyExecutes(const std::string &status) {
    return status == "Accepted" || status == "Approved";
}

bool openStatus(const std::string &status) {
    const auto normalized = lowerString(status);
    return normalized == "pending" || normalized == "accepted";
}

StatusDecision decideStatus(const std::string &requestedStatus,
                            bool requiresApproval,
                            bool actorInvolved,
                            bool actorCommissioner,
                            bool enforceParticipantRules) {
    StatusDecision decision;
    const auto requested = lowerString(requestedStatus);

    if (requested == "accepted") {
        if (enforceParticipantRules && !actorInvolved) {
            return decision;
        }
        decision.allowed = true;
        decision.execute = !requiresApproval;
        decision.databaseStatus = decision.execute ? "approved" : "accepted";
        decision.displayStatus = displayStatusFor(decision.databaseStatus);
        return decision;
    }

    if (requested == "approved" || requested == "vetoed") {
        if (!actorCommissioner) {
            decision.commissionerRequired = true;
            return decision;
        }
        decision.allowed = true;
        decision.execute = requested == "approved";
        decision.databaseStatus = requested;
        decision.displayStatus = displayStatusFor(requested);
        return decision;
    }

    if (requested == "declined" || requested == "cancelled") {
        if (enforceParticipantRules && !actorInvolved && !actorCommissioner) {
            return decision;
        }
        decision.allowed = true;
        decision.databaseStatus = requested;
        decision.displayStatus = displayStatusFor(requested);
        return decision;
    }

    return decision;
}

bool playerLockedInOpenOffer(const Json::Value &offers,
                             const std::string &managerEmail,
                             const std::string &playerId) {
    if (!offers.isArray() || managerEmail.empty() || playerId.empty()) {
        return false;
    }

    for (const auto &offer : offers) {
        if (!offer.isObject() || !openStatus(stringField(offer, "status"))) {
            continue;
        }
        if (stringField(offer, "offeredByEmail") == managerEmail
            && stringField(offer["offerPlayer"], "id") == playerId) {
            return true;
        }
        if (stringField(offer, "offeredToEmail") == managerEmail
            && stringField(offer["requestPlayer"], "id") == playerId) {
            return true;
        }
    }
    return false;
}

std::string offerTransactionSummary(const Json::Value &player) {
    return "Offered " + stringField(player, "name");
}

std::string statusTransactionSummary(const std::string &displayStatus,
                                     const Json::Value &player) {
    return displayStatus + ": " + stringField(player, "name");
}

} // namespace cff::league_trade
