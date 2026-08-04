Json::Value tradeStatePayload(PGconn *connection,
                              const std::string &leagueId,
                              const std::string &email,
                              const TradeAccess &access,
                              long long version,
                              int expiredCount = 0) {
    Json::Value payload(Json::objectValue);
    payload["leagueId"] = leagueId;
    payload["version"] = Json::Int64(version);
    payload["offers"] = tradeOffersPayload(connection, leagueId, email, access.commissioner);
    payload["roster"] = rosterPayload(connection, leagueId, email);
    payload["tradeRules"] = access.tradeRules;
    payload["commissioner"] = access.commissioner;
    payload["expiredCount"] = expiredCount;
    payload["lineupLocked"] = lineupLocked(connection, leagueId);
    return payload;
}

Json::Value currentTradeStateDetails(PGconn *connection,
                                     const std::string &leagueId,
                                     const std::string &email,
                                     const TradeAccess &access,
                                     long long version) {
    Json::Value details(Json::objectValue);
    details["currentVersion"] = Json::Int64(version);
    details["state"] = tradeStatePayload(connection, leagueId, email, access, version);
    return details;
}

std::optional<Json::Value> tradeFromState(const Json::Value &state,
                                          const std::string &tradeId) {
    const auto &offers = state["offers"];
    if (!offers.isArray()) return std::nullopt;
    for (const auto &offer : offers) {
        if (offer.get("id", "").asString() == tradeId) return offer;
    }
    return std::nullopt;
}

drogon::HttpResponsePtr tradeStorageUnavailable() {
    return errorResponse(drogon::k503ServiceUnavailable,
                         "Trade storage is temporarily unavailable.",
                         "trade_storage_unavailable",
                         true);
}

drogon::HttpResponsePtr tradeStateResponse(const Json::Value &payload,
                                           bool legacyCollection = false,
                                           const std::string &legacyTradeId = "",
                                           drogon::HttpStatusCode status = drogon::k200OK) {
    if (legacyCollection) return jsonResponse(payload["offers"], status);
    const bool replay = payload.get("idempotentReplay", false).asBool();
    if (!legacyTradeId.empty() && !replay) {
        if (const auto offer = tradeFromState(payload, legacyTradeId)) {
            return jsonResponse(*offer, status);
        }
    }
    return jsonResponse(payload, status);
}
