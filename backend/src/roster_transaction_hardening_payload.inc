Json::Value rosterStatePayload(PGconn *connection,
                               const std::string &leagueId,
                               const std::string &email,
                               const LeagueAccess &access,
                               long long knownVersion = -1) {
    Json::Value payload(Json::objectValue);
    const auto roster = rosterPayload(connection, leagueId, email);
    const auto version = knownVersion >= 0
        ? knownVersion
        : rosterVersion(connection, leagueId, email);
    payload["leagueId"] = leagueId;
    payload["managerEmail"] = email;
    payload["version"] = Json::Int64(version);
    payload["roster"] = roster;
    payload["rosterCount"] = static_cast<Json::UInt64>(roster.size());
    payload["rosterLimit"] = cff::league_roster::rosterLimitFromRules(access.rosterRules);
    payload["waiverMode"] = access.waiverRules.get("mode", "free_agency").asString();
    payload["freeAgencyLocked"] = !cff::roster_transaction::directAcquisitionAllowed(access.waiverRules);
    payload["directAcquisitionAllowed"] = cff::roster_transaction::directAcquisitionAllowed(access.waiverRules);
    payload["unavailablePlayerIds"] = unavailablePlayerIds(connection, leagueId);
    return payload;
}

drogon::HttpResponsePtr rosterStateResponse(const Json::Value &payload,
                                             bool legacyArray,
                                             drogon::HttpStatusCode status = drogon::k200OK) {
    if (legacyArray) return jsonResponse(payload["roster"], status);
    return jsonResponse(payload, status);
}

Json::Value currentStateDetails(PGconn *connection,
                                const std::string &leagueId,
                                const std::string &email,
                                const LeagueAccess &access) {
    Json::Value details(Json::objectValue);
    details["currentVersion"] = Json::Int64(rosterVersion(connection, leagueId, email));
    details["state"] = rosterStatePayload(connection, leagueId, email, access);
    return details;
}
