#include "trade_lifecycle.h"

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace cff::trade_lifecycle {

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

} // namespace

std::string canonicalEmail(std::string value) {
    return lower(trim(std::move(value)));
}

std::string canonicalPlayerId(std::string value) {
    return trim(std::move(value));
}

bool openStatus(const std::string &status) {
    const auto normalized = lower(trim(status));
    return normalized == "pending" || normalized == "accepted";
}

bool terminalStatus(const std::string &status) {
    const auto normalized = lower(trim(status));
    return normalized == "approved"
        || normalized == "declined"
        || normalized == "vetoed"
        || normalized == "cancelled"
        || normalized == "expired";
}

bool expectedVersionMatches(long long currentVersion,
                            const Json::Value &body,
                            bool required) {
    if (!body.isObject() || !body.isMember("expectedVersion")) {
        return !required;
    }
    return normalizedVersion(body["expectedVersion"], -1) == currentVersion;
}

TransitionDecision decideTransition(const std::string &currentStatus,
                                    const std::string &requestedStatus,
                                    const std::string &actorEmail,
                                    const std::string &offeredByEmail,
                                    const std::string &offeredToEmail,
                                    bool actorCommissioner,
                                    bool requiresApproval) {
    TransitionDecision decision;
    const auto current = lower(trim(currentStatus));
    const auto requested = lower(trim(requestedStatus));
    const auto actor = canonicalEmail(actorEmail);
    const auto offeredBy = canonicalEmail(offeredByEmail);
    const auto offeredTo = canonicalEmail(offeredToEmail);

    if (!openStatus(current)) {
        decision.errorCode = "trade_closed";
        return decision;
    }

    if (requested == "accepted" || requested == "accept") {
        if (current != "pending" || actor != offeredTo) {
            decision.errorCode = actor == offeredTo ? "trade_transition_conflict" : "trade_recipient_required";
            return decision;
        }
        decision.allowed = true;
        decision.execute = !requiresApproval;
        decision.releaseLocks = !requiresApproval;
        decision.nextStatus = requiresApproval ? "accepted" : "approved";
        return decision;
    }

    if (requested == "approved" || requested == "approve") {
        if (!actorCommissioner) {
            decision.errorCode = "commissioner_required";
            return decision;
        }
        if (!requiresApproval || current != "accepted") {
            decision.errorCode = "trade_not_awaiting_approval";
            return decision;
        }
        decision.allowed = true;
        decision.execute = true;
        decision.releaseLocks = true;
        decision.nextStatus = "approved";
        return decision;
    }

    if (requested == "vetoed" || requested == "veto") {
        if (!actorCommissioner) {
            decision.errorCode = "commissioner_required";
            return decision;
        }
        if (requiresApproval && current != "accepted") {
            decision.errorCode = "trade_not_awaiting_approval";
            return decision;
        }
        decision.allowed = true;
        decision.releaseLocks = true;
        decision.nextStatus = "vetoed";
        return decision;
    }

    if (requested == "declined" || requested == "decline") {
        if (actor != offeredTo && !actorCommissioner) {
            decision.errorCode = "trade_recipient_required";
            return decision;
        }
        decision.allowed = true;
        decision.releaseLocks = true;
        decision.nextStatus = "declined";
        return decision;
    }

    if (requested == "cancelled" || requested == "canceled" || requested == "cancel") {
        if (actor != offeredBy && !actorCommissioner) {
            decision.errorCode = "trade_offerer_required";
            return decision;
        }
        decision.allowed = true;
        decision.releaseLocks = true;
        decision.nextStatus = "cancelled";
        return decision;
    }

    decision.errorCode = "invalid_trade_status";
    return decision;
}

bool validOfferPlayers(const std::string &offeredPlayerId,
                       const std::string &requestedPlayerId) {
    return validOfferPlayerPackages({offeredPlayerId}, {requestedPlayerId}, 1);
}

bool validOfferPlayerPackages(const std::vector<std::string> &offeredPlayerIds,
                              const std::vector<std::string> &requestedPlayerIds,
                              std::size_t maximumPerSide) {
    if (maximumPerSide == 0
        || offeredPlayerIds.empty()
        || requestedPlayerIds.empty()
        || offeredPlayerIds.size() > maximumPerSide
        || requestedPlayerIds.size() > maximumPerSide) {
        return false;
    }

    std::unordered_set<std::string> players;
    players.reserve(offeredPlayerIds.size() + requestedPlayerIds.size());
    const auto appendPackage = [&players](const std::vector<std::string> &ids) {
        for (const auto &rawId : ids) {
            const auto id = canonicalPlayerId(rawId);
            if (id.empty() || !players.insert(id).second) return false;
        }
        return true;
    };
    return appendPackage(offeredPlayerIds) && appendPackage(requestedPlayerIds);
}

} // namespace cff::trade_lifecycle
