constexpr int kAutoDraftMissThreshold = 2;
constexpr int kDisconnectGraceSeconds = 60;

struct AutoDraftCandidate {
    Json::Value player{Json::objectValue};
    std::string playerId;
    std::string rosterSlot;
    std::string source;
};

bool logDraftActivity(PGconn *connection,
                      const std::string &leagueId,
                      const std::string &managerEmail,
                      const std::string &eventType,
                      const std::string &message,
                      int pickNumber = 0,
                      const std::string &playerId = "",
                      const Json::Value &metadata = Json::Value{Json::objectValue}) {
    return commandOk(execute(connection,
        "INSERT INTO draft_activity_log "
        "(league_id, manager_email, event_type, message, pick_number, player_id, metadata) "
        "VALUES ($1, $2, $3, $4, NULLIF($5, '')::integer, NULLIF($6, ''), $7::jsonb)",
        {leagueId,
         managerEmail,
         eventType,
         message,
         pickNumber > 0 ? std::to_string(pickNumber) : "",
         playerId,
         jsonToString(metadata)}));
}

std::unordered_set<std::string> draftedPlayerIds(PGconn *connection, const std::string &leagueId) {
    std::unordered_set<std::string> ids;
    auto result = execute(connection,
        "SELECT player_id FROM draft_picks WHERE league_id = $1",
        {leagueId});
    if (!tuplesOk(result)) return ids;
    for (int row = 0; row < PQntuples(result.get()); ++row) ids.insert(cell(result.get(), row, 0));
    return ids;
}

Json::Value playerSnapshotFromCatalog(PGresult *result, int row, int rank) {
    Json::Value player(Json::objectValue);
    player["id"] = cell(result, row, 0);
    player["name"] = cell(result, row, 1);
    player["team"] = cell(result, row, 2);
    player["position"] = cell(result, row, 3);
    player["conference"] = cell(result, row, 4);
    player["class"] = cell(result, row, 5);
    player["rank"] = rank;
    return player;
}

std::optional<AutoDraftCandidate> eligibleCandidateFromPlayer(
    const Json::Value &player,
    const std::string &source,
    const Json::Value &rules,
    const std::unordered_map<std::string, int> &counts,
    const std::unordered_set<std::string> &drafted) {
    const auto playerId = trim(player.get("id", "").asString());
    if (playerId.empty() || drafted.find(playerId) != drafted.end()) return std::nullopt;
    const auto slot = cff::league_roster::preferredRosterSlot(player, rules, counts);
    if (!slot) return std::nullopt;
    AutoDraftCandidate candidate;
    candidate.player = player;
    candidate.playerId = playerId;
    candidate.rosterSlot = *slot;
    candidate.source = source;
    return candidate;
}

std::optional<AutoDraftCandidate> queuedAutoDraftCandidate(
    PGconn *connection,
    const std::string &leagueId,
    const std::string &managerEmail,
    const Json::Value &rules,
    const std::unordered_map<std::string, int> &counts,
    const std::unordered_set<std::string> &drafted) {
    auto result = execute(connection,
        "SELECT queue::text FROM draft_queues WHERE league_id = $1 AND lower(manager_email) = lower($2)",
        {leagueId, managerEmail});
    if (!tuplesOk(result) || PQntuples(result.get()) == 0) return std::nullopt;
    const auto queue = jsonFromString(cell(result.get(), 0, 0), Json::Value{Json::arrayValue});
    if (!queue.isArray()) return std::nullopt;
    for (const auto &entry : queue) {
        if (!entry.isObject()) continue;
        if (const auto candidate = eligibleCandidateFromPlayer(entry, "personal_queue", rules, counts, drafted)) {
            return candidate;
        }
    }
    return std::nullopt;
}

std::optional<AutoDraftCandidate> systemAutoDraftCandidate(
    PGconn *connection,
    const std::string &leagueId,
    const Json::Value &rules,
    const std::unordered_map<std::string, int> &counts,
    const std::unordered_set<std::string> &drafted) {
    auto result = execute(connection,
        "SELECT p.id, p.full_name, COALESCE(p.team, ''), COALESCE(p.position, ''), "
        "COALESCE(p.conference, ''), COALESCE(p.year, '') "
        "FROM players p WHERE UPPER(COALESCE(p.position, '')) IN ('QB', 'RB', 'WR', 'TE', 'K') "
        "AND NOT EXISTS (SELECT 1 FROM draft_picks dp WHERE dp.league_id = $1 AND dp.player_id = p.id) "
        "ORDER BY CASE UPPER(COALESCE(p.position, '')) "
        "WHEN 'QB' THEN 1 WHEN 'RB' THEN 2 WHEN 'WR' THEN 3 WHEN 'TE' THEN 4 WHEN 'K' THEN 5 ELSE 9 END, "
        "LOWER(p.full_name), p.id LIMIT 250",
        {leagueId});
    if (!tuplesOk(result)) return std::nullopt;
    for (int row = 0; row < PQntuples(result.get()); ++row) {
        auto player = playerSnapshotFromCatalog(result.get(), row, row + 1);
        if (const auto candidate = eligibleCandidateFromPlayer(player, "system_ranking", rules, counts, drafted)) {
            return candidate;
        }
    }
    return std::nullopt;
}

std::optional<AutoDraftCandidate> selectAutoDraftCandidate(PGconn *connection,
                                                           const std::string &leagueId,
                                                           const std::string &managerEmail,
                                                           const Json::Value &rules) {
    const auto counts = rosterCounts(connection, leagueId, managerEmail);
    const auto drafted = draftedPlayerIds(connection, leagueId);
    if (const auto queued = queuedAutoDraftCandidate(connection, leagueId, managerEmail, rules, counts, drafted)) {
        return queued;
    }
    return systemAutoDraftCandidate(connection, leagueId, rules, counts, drafted);
}

std::optional<long long> persistDraftSelection(PGconn *connection,
                                               const std::string &leagueId,
                                               const std::string &managerEmail,
                                               int pickNumber,
                                               int clockSeconds,
                                               const Json::Value &rules,
                                               int managerCount,
                                               const AutoDraftCandidate &candidate,
                                               const std::string &selectionSource,
                                               const std::string &idempotencyKey,
                                               bool automatic) {
    const auto pickId = leagueId + "-pick-" + std::to_string(pickNumber);
    auto insertedPick = execute(connection,
        "INSERT INTO draft_picks "
        "(id, league_id, manager_email, pick_number, player_id, player_snapshot, selection_source, idempotency_key) "
        "VALUES ($1, $2, $3, $4::integer, $5, $6::jsonb, $7, NULLIF($8, ''))",
        {pickId,
         leagueId,
         managerEmail,
         std::to_string(pickNumber),
         candidate.playerId,
         jsonToString(candidate.player),
         selectionSource,
         idempotencyKey});
    if (!commandOk(insertedPick)) return std::nullopt;
    auto insertedRoster = execute(connection,
        "INSERT INTO rosters (league_id, manager_email, player_id, player_snapshot, roster_slot, acquired_via) "
        "VALUES ($1, $2, $3, $4::jsonb, $5, 'draft')",
        {leagueId, managerEmail, candidate.playerId, jsonToString(candidate.player), candidate.rosterSlot});
    if (!commandOk(insertedRoster)) return std::nullopt;

    if (automatic) {
        (void)execute(connection,
            "INSERT INTO draft_readiness "
            "(league_id, manager_email, ready, auto_draft_enabled, consecutive_missed_picks, last_seen_at, updated_at) "
            "VALUES ($1, $2, FALSE, FALSE, 1, NOW(), NOW()) "
            "ON CONFLICT (league_id, manager_email) DO UPDATE SET "
            "consecutive_missed_picks = draft_readiness.consecutive_missed_picks + 1, "
            "auto_draft_enabled = CASE "
            "WHEN draft_readiness.consecutive_missed_picks + 1 >= $3::integer THEN TRUE "
            "ELSE draft_readiness.auto_draft_enabled END, updated_at = NOW()",
            {leagueId, managerEmail, std::to_string(kAutoDraftMissThreshold)});
    } else {
        (void)execute(connection,
            "UPDATE draft_readiness SET consecutive_missed_picks = 0, updated_at = NOW() "
            "WHERE league_id = $1 AND lower(manager_email) = lower($2)",
            {leagueId, managerEmail});
    }

    const bool complete = cff::draft_lifecycle::draftCompleteAfterPick(
        pickNumber,
        managerCount,
        rules);
    auto updated = execute(connection,
        "UPDATE draft_states SET status = $2, current_pick = $3::integer, "
        "pick_deadline = CASE WHEN $2 = 'complete' THEN NULL "
        "ELSE NOW() + ($4::integer * INTERVAL '1 second') END, "
        "completed_at = CASE WHEN $2 = 'complete' THEN NOW() ELSE NULL END, "
        "version = version + 1, updated_at = NOW() WHERE league_id = $1 RETURNING version",
        {leagueId,
         complete ? "complete" : "open",
         std::to_string(pickNumber + 1),
         std::to_string(clockSeconds)});
    if (!tuplesOk(updated) || PQntuples(updated.get()) == 0) return std::nullopt;
    const auto version = cellInt64(updated.get(), 0, 0, 0);
    (void)logDraftActivity(
        connection,
        leagueId,
        managerEmail,
        automatic ? "auto_pick" : "manual_pick",
        automatic ? "Auto-draft selected " + candidate.player.get("name", Json::Value(candidate.playerId)).asString() + "."
                  : "Player selected manually.",
        pickNumber,
        candidate.playerId);
    if (complete) {
        (void)logDraftActivity(connection, leagueId, "", "draft_completed", "Draft completed.", pickNumber);
    }
    return version;
}

bool shouldAutoDraftCurrentPick(PGconn *connection,
                                const std::string &leagueId,
                                const std::string &managerEmail,
                                bool &deadlineExpired,
                                bool &autoDraftEnabled,
                                bool &disconnectExpired) {
    auto result = execute(connection,
        "SELECT COALESCE(ds.pick_deadline <= NOW(), FALSE), "
        "COALESCE(dr.auto_draft_enabled, FALSE), "
        "COALESCE(dr.last_seen_at < NOW() - ($3::integer * INTERVAL '1 second'), FALSE) "
        "FROM draft_states ds LEFT JOIN draft_readiness dr "
        "ON dr.league_id = ds.league_id AND lower(dr.manager_email) = lower($2) "
        "WHERE ds.league_id = $1",
        {leagueId, managerEmail, std::to_string(kDisconnectGraceSeconds)});
    if (!tuplesOk(result) || PQntuples(result.get()) == 0) return false;
    deadlineExpired = cell(result.get(), 0, 0) == "t";
    autoDraftEnabled = cell(result.get(), 0, 1) == "t";
    disconnectExpired = cell(result.get(), 0, 2) == "t";
    return deadlineExpired || autoDraftEnabled || disconnectExpired;
}

int resolveDueAutoDrafts(PGconn *connection, const std::string &leagueId) {
    int resolved = 0;
    for (int attempt = 0; attempt < 32; ++attempt) {
        auto state = execute(connection,
            "SELECT ds.status, ds.current_pick, to_json(ds.draft_order)::text, ds.pick_clock_seconds, "
            "l.draft_type, l.roster_rules::text "
            "FROM draft_states ds JOIN leagues l ON l.id = ds.league_id "
            "WHERE ds.league_id = $1 FOR UPDATE OF ds",
            {leagueId});
        if (!tuplesOk(state) || PQntuples(state.get()) == 0 || cell(state.get(), 0, 0) != "open") break;
        const int currentPick = cellInt(state.get(), 0, 1, 1);
        const auto order = jsonFromString(cell(state.get(), 0, 2), Json::Value{Json::arrayValue});
        const int clockSeconds = cellInt(state.get(), 0, 3, 90);
        const auto draftType = cell(state.get(), 0, 4).empty() ? "snake" : cell(state.get(), 0, 4);
        const auto rules = jsonFromString(cell(state.get(), 0, 5));
        const auto currentManager = cff::draft_lifecycle::managerForPick(order, currentPick, draftType);
        if (currentManager.empty()) break;

        bool deadlineExpired = false;
        bool autoDraftEnabled = false;
        bool disconnectExpired = false;
        if (!shouldAutoDraftCurrentPick(
                connection, leagueId, currentManager, deadlineExpired, autoDraftEnabled, disconnectExpired)) {
            break;
        }

        Json::Value metadata(Json::objectValue);
        metadata["deadlineExpired"] = deadlineExpired;
        metadata["autoDraftEnabled"] = autoDraftEnabled;
        metadata["disconnectExpired"] = disconnectExpired;
        (void)logDraftActivity(
            connection,
            leagueId,
            currentManager,
            deadlineExpired ? "timer_expired" : autoDraftEnabled ? "auto_draft_active" : "manager_inactive",
            deadlineExpired ? "Pick timer expired." : autoDraftEnabled ? "Auto-draft mode is active." : "Manager inactive past grace period.",
            currentPick,
            "",
            metadata);

        const auto candidate = selectAutoDraftCandidate(connection, leagueId, currentManager, rules);
        if (!candidate) break;
        const auto key = "auto:" + leagueId + ":" + std::to_string(currentPick);
        if (!persistDraftSelection(
                connection,
                leagueId,
                currentManager,
                currentPick,
                clockSeconds,
                rules,
                static_cast<int>(order.size()),
                *candidate,
                candidate->source,
                key,
                true)) {
            break;
        }
        ++resolved;
    }
    return resolved;
}
