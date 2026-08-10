std::optional<Json::Value> canonicalDraftPlayer(PGconn *connection,
                                                const std::string &playerId) {
    const auto normalizedId = trim(playerId);
    if (normalizedId.empty()) return std::nullopt;
    auto result = execute(connection,
        "SELECT p.id, p.full_name, COALESCE(p.team, ''), COALESCE(p.position, ''), "
        "COALESCE(p.conference, ''), COALESCE(p.year, ''), COALESCE(p.season, 0), "
        "COALESCE(to_char(p.updated_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), '') "
        "FROM players p WHERE p.id = $1 AND COALESCE(p.active, TRUE) = TRUE LIMIT 1",
        {normalizedId});
    if (!tuplesOk(result) || PQntuples(result.get()) == 0) return std::nullopt;

    Json::Value player(Json::objectValue);
    player["id"] = cell(result.get(), 0, 0);
    player["name"] = cell(result.get(), 0, 1);
    player["fullName"] = cell(result.get(), 0, 1);
    player["team"] = cell(result.get(), 0, 2);
    player["position"] = cell(result.get(), 0, 3);
    player["conference"] = cell(result.get(), 0, 4);
    player["class"] = cell(result.get(), 0, 5);
    player["year"] = cell(result.get(), 0, 5);
    player["season"] = cellInt(result.get(), 0, 6, 0);
    player["updatedAt"] = cell(result.get(), 0, 7);
    return player;
}

std::string requestedDraftPlayerId(const Json::Value &body) {
    if (body.isMember("playerId") && body["playerId"].isString()) {
        return trim(body["playerId"].asString());
    }
    if (body.isMember("player") && body["player"].isObject()) {
        return trim(body["player"].get("id", "").asString());
    }
    return "";
}
