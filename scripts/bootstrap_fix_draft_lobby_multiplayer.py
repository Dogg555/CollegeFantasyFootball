#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HANDLER = ROOT / "backend/src/handlers/league_handler.cpp"
HEADER = ROOT / "backend/src/handlers/league_handler.h"
ROUTES = ROOT / "backend/src/league_routes.cpp"
STATE = ROOT / "frontend/state.js"
DRAFT = ROOT / "frontend/draft.js"
DRAFT_HTML = ROOT / "frontend/draft.html"
LEAGUE = ROOT / "frontend/league.js"
LEAGUE_HTML = ROOT / "frontend/league.html"


def replace_once(text, old, new, description):
    count = text.count(old)
    if count == 1:
        return text.replace(old, new, 1)
    if count == 0 and new in text:
        return text
    raise RuntimeError(f"expected one {description}, found {count}")


def replace_function(text, signature, replacement):
    start = text.find(signature)
    if start < 0:
        if replacement.strip() in text:
            return text
        raise RuntimeError(f"function signature not found: {signature}")
    brace = text.find("{", start + len(signature))
    if brace < 0:
        raise RuntimeError(f"function body not found: {signature}")
    depth = 0
    end = brace
    while end < len(text):
        if text[end] == "{":
            depth += 1
        elif text[end] == "}":
            depth -= 1
            if depth == 0:
                end += 1
                while end < len(text) and text[end] in " \t":
                    end += 1
                if text.startswith("\r\n", end):
                    end += 2
                elif end < len(text) and text[end] == "\n":
                    end += 1
                if text.startswith("\r\n", end):
                    end += 2
                elif end < len(text) and text[end] == "\n":
                    end += 1
                return text[:start] + replacement.rstrip() + "\n\n" + text[end:]
        end += 1
    raise RuntimeError(f"unterminated function: {signature}")


handler = HANDLER.read_text(encoding="utf-8")

map_anchor = "std::unordered_map<std::string, Json::Value> draftQueuesByLeagueManager;\n"
if "draftStateByLeague" not in handler:
    handler = replace_once(
        handler,
        map_anchor,
        map_anchor + "std::unordered_map<std::string, Json::Value> draftStateByLeague;\n",
        "local draft state map anchor",
    )

time_anchor = '''double projectionForPlayer(const Json::Value &player) {
    if (player.isMember("projection") && player["projection"].isNumeric()) {
        return player["projection"].asDouble();
    }
    if (player.isMember("projectedPoints") && player["projectedPoints"].isNumeric()) {
        return player["projectedPoints"].asDouble();
    }
    return 0.0;
}
'''
time_helpers = '''double projectionForPlayer(const Json::Value &player) {
    if (player.isMember("projection") && player["projection"].isNumeric()) {
        return player["projection"].asDouble();
    }
    if (player.isMember("projectedPoints") && player["projectedPoints"].isNumeric()) {
        return player["projectedPoints"].asDouble();
    }
    return 0.0;
}

std::string draftIsoTimestamp(std::chrono::system_clock::time_point value) {
    const auto raw = std::chrono::system_clock::to_time_t(value);
    std::tm timeInfo{};
#ifdef _WIN32
    gmtime_s(&timeInfo, &raw);
#else
    gmtime_r(&raw, &timeInfo);
#endif
    std::ostringstream out;
    out << std::put_time(&timeInfo, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

std::string draftIsoNow() {
    return draftIsoTimestamp(std::chrono::system_clock::now());
}

std::string draftDeadlineFromNow(int seconds) {
    return draftIsoTimestamp(std::chrono::system_clock::now() + std::chrono::seconds(std::max(1, seconds)));
}
'''
if "std::string draftIsoTimestamp" not in handler:
    handler = replace_once(handler, time_anchor, time_helpers, "draft timestamp helper anchor")

local_anchor = '''Json::Value tradeRulesForLeagueLocked(const std::string &leagueId) {
    const auto it = leaguesById.find(leagueId);
    if (it != leaguesById.end() && it->second.league.tradeRules.isObject()) {
        return it->second.league.tradeRules;
    }
    Json::Value rules(Json::objectValue);
    rules["commissionerApproval"] = false;
    rules["expirationHours"] = 48;
    return rules;
}
'''
local_helpers = local_anchor + '''
Json::Value activeDraftOrderLocked(const std::string &leagueId) {
    Json::Value order(Json::arrayValue);
    for (const auto &member : arrayForLeague(membersByLeague, leagueId)) {
        if (lowerString(jsonString(member, "status")) != "active") continue;
        const auto email = canonicalEmail(jsonString(member, "email"));
        if (!email.empty()) order.append(email);
    }
    return order;
}

bool localDraftOrderMatchesActiveMembers(const std::string &leagueId,
                                         const Json::Value &order) {
    const auto active = activeDraftOrderLocked(leagueId);
    if (!order.isArray() || order.empty() || order.size() != active.size()) return false;
    std::unordered_set<std::string> activeEmails;
    for (const auto &email : active) activeEmails.insert(canonicalEmail(email.asString()));
    std::unordered_set<std::string> seen;
    for (const auto &value : order) {
        if (!value.isString()) return false;
        const auto email = canonicalEmail(value.asString());
        if (email.empty() || activeEmails.find(email) == activeEmails.end() || !seen.insert(email).second) {
            return false;
        }
    }
    return true;
}

Json::Value &localDraftStateLocked(const std::string &leagueId) {
    auto &state = draftStateByLeague[leagueId];
    if (!state.isObject()) {
        state = Json::Value{Json::objectValue};
        state["status"] = "not_started";
        state["currentPick"] = 1;
        state["pickClockSeconds"] = 90;
        state["pickDeadline"] = "";
        state["startedAt"] = "";
        state["draftOrder"] = activeDraftOrderLocked(leagueId);
    }
    if (!state["draftOrder"].isArray() || state["draftOrder"].empty()) {
        state["draftOrder"] = activeDraftOrderLocked(leagueId);
    }
    const auto leagueIt = leaguesById.find(leagueId);
    state["lobbyOpen"] = leagueIt != leaguesById.end() && leagueIt->second.league.draftLobbyOpen;
    state["draftType"] = leagueIt != leaguesById.end()
        ? leagueIt->second.league.draftType
        : "snake";
    return state;
}

Json::Value localDraftPayloadLocked(const std::string &accountEmail,
                                    const std::string &leagueId) {
    auto &state = localDraftStateLocked(leagueId);
    Json::Value payload = state;
    payload["queue"] = arrayForLeague(draftQueuesByLeagueManager, leagueId + ":" + accountEmail);
    payload["picks"] = arrayForLeague(draftPicksByLeague, leagueId);
    payload["currentManager"] = cff::league_schedule::currentDraftManager(
        payload["draftOrder"],
        payload.get("currentPick", 1).asInt(),
        payload.get("draftType", "snake").asString());
    return payload;
}
'''
if "Json::Value activeDraftOrderLocked" not in handler:
    handler = replace_once(handler, local_anchor, local_helpers, "local draft helper anchor")

handler = replace_function(handler, "Json::Value draftOrderForLeague(PGconn *conn, const std::string &leagueId)", '''Json::Value activeDraftOrderForLeague(PGconn *conn, const std::string &leagueId) {
    auto members = membersForLeague(conn, leagueId);
    Json::Value order(Json::arrayValue);
    for (const auto &member : members) {
        if (lowerString(jsonString(member, "status")) != "active") continue;
        const auto email = canonicalEmail(jsonString(member, "email"));
        if (!email.empty()) order.append(email);
    }
    return order;
}

Json::Value draftOrderForLeague(PGconn *conn, const std::string &leagueId) {
    auto result = execParams(conn,
                             "SELECT to_json(draft_order)::text FROM draft_states WHERE league_id = $1",
                             {leagueId});
    if (resultOk(result.get(), PGRES_TUPLES_OK) && PQntuples(result.get()) > 0) {
        const auto stored = jsonFromString(cell(result.get(), 0, 0), Json::Value{Json::arrayValue});
        if (stored.isArray() && !stored.empty()) return stored;
    }
    return activeDraftOrderForLeague(conn, leagueId);
}''')

handler = replace_function(handler, "bool dbDraftComplete(PGconn *conn, const std::string &leagueId)", '''bool dbDraftComplete(PGconn *conn, const std::string &leagueId) {
    const auto limit = dbRosterLimit(leagueId).value_or(14);
    auto members = membersForLeague(conn, leagueId);
    int activeMembers = 0;
    for (const auto &member : members) {
        if (lowerString(jsonString(member, "status")) != "active") continue;
        const auto email = jsonString(member, "email");
        if (email.empty()) continue;
        ++activeMembers;
        auto result = execParams(conn,
                                 "SELECT COUNT(*) FROM rosters WHERE league_id = $1 AND manager_email = $2",
                                 {leagueId, email});
        if (!resultOk(result.get(), PGRES_TUPLES_OK) || cellInt(result.get(), 0, 0, 0) < limit) {
            return false;
        }
    }
    return activeMembers > 0;
}''')

handler = replace_function(handler, "std::optional<Json::Value> dbGetDraftState(const std::string &accountEmail, const std::string &leagueId)", '''std::optional<Json::Value> dbGetDraftState(const std::string &accountEmail, const std::string &leagueId) {
    if (!dbCanAccessLeague(accountEmail, leagueId)) return std::nullopt;
    auto conn = connectToDb();
    if (!conn) return std::nullopt;
    auto state = execParams(conn.get(),
                            "INSERT INTO draft_states (league_id, status, current_pick, draft_order, pick_deadline, started_at) "
                            "VALUES ($1, 'not_started', 1, ARRAY(SELECT email FROM league_members WHERE league_id = $1 AND status = 'active' ORDER BY role, created_at), NULL, NULL) "
                            "ON CONFLICT (league_id) DO NOTHING",
                            {leagueId});
    (void)state;
    auto queueResult = execParams(conn.get(),
                                  "SELECT queue::text FROM draft_queues WHERE league_id = $1 AND manager_email = $2",
                                  {leagueId, accountEmail});
    Json::Value payload;
    payload["queue"] = Json::Value{Json::arrayValue};
    if (resultOk(queueResult.get(), PGRES_TUPLES_OK) && PQntuples(queueResult.get()) > 0) {
        payload["queue"] = jsonFromString(cell(queueResult.get(), 0, 0), Json::Value{Json::arrayValue});
    }
    auto stateResult = execParams(conn.get(),
                                  "SELECT status, current_pick, pick_clock_seconds, "
                                  "COALESCE(to_char(pick_deadline AT TIME ZONE 'UTC', 'YYYY-MM-DD\\\"T\\\"HH24:MI:SS\\\"Z\\\"'), ''), "
                                  "COALESCE(to_char(started_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\\\"T\\\"HH24:MI:SS\\\"Z\\\"'), '') "
                                  "FROM draft_states WHERE league_id = $1",
                                  {leagueId});
    payload["status"] = "not_started";
    payload["currentPick"] = 1;
    payload["pickClockSeconds"] = 90;
    payload["pickDeadline"] = "";
    payload["startedAt"] = "";
    if (resultOk(stateResult.get(), PGRES_TUPLES_OK) && PQntuples(stateResult.get()) > 0) {
        payload["status"] = cell(stateResult.get(), 0, 0);
        payload["currentPick"] = cellInt(stateResult.get(), 0, 1, 1);
        payload["pickClockSeconds"] = cellInt(stateResult.get(), 0, 2, 90);
        payload["pickDeadline"] = cell(stateResult.get(), 0, 3);
        payload["startedAt"] = cell(stateResult.get(), 0, 4);
    }
    payload["lobbyOpen"] = dbDraftLobbyOpen(conn.get(), leagueId);
    payload["draftType"] = draftTypeForLeague(conn.get(), leagueId);
    payload["draftOrder"] = draftOrderForLeague(conn.get(), leagueId);
    payload["currentManager"] = cff::league_schedule::currentDraftManager(
        payload["draftOrder"], payload["currentPick"].asInt(), payload["draftType"].asString());
    payload["picks"] = draftPicksForLeague(conn.get(), leagueId);
    if (payload["status"].asString() != "not_started" && dbDraftComplete(conn.get(), leagueId)) {
        payload["status"] = "complete";
    }
    return payload;
}''')

handler = replace_function(handler, "std::optional<Json::Value> dbSaveDraftOrder(const std::string &accountEmail,", '''std::optional<Json::Value> dbSaveDraftOrder(const std::string &accountEmail,
                                            const std::string &leagueId,
                                            const Json::Value &draftOrder) {
    if (!dbIsCommissioner(accountEmail, leagueId)) return std::nullopt;
    auto conn = connectToDb();
    if (!conn) return std::nullopt;
    if (!draftOrderMatchesMembers(conn.get(), leagueId, draftOrder)) return std::nullopt;
    auto picks = execParams(conn.get(),
                            "SELECT COUNT(*) FROM draft_picks WHERE league_id = $1",
                            {leagueId});
    if (!resultOk(picks.get(), PGRES_TUPLES_OK) || cellInt(picks.get(), 0, 0, 0) > 0) return std::nullopt;
    auto insertState = execParams(conn.get(),
                                  "INSERT INTO draft_states (league_id, status, current_pick, draft_order, pick_deadline, started_at) "
                                  "VALUES ($1, 'not_started', 1, ARRAY(SELECT jsonb_array_elements_text($2::jsonb)), NULL, NULL) "
                                  "ON CONFLICT (league_id) DO NOTHING",
                                  {leagueId, jsonToString(draftOrder)});
    auto updateState = execParams(conn.get(),
                                  "UPDATE draft_states SET draft_order = ARRAY(SELECT jsonb_array_elements_text($2::jsonb)), "
                                  "current_pick = 1, pick_deadline = NULL, started_at = NULL, updated_at = NOW() "
                                  "WHERE league_id = $1 AND status = 'not_started'",
                                  {leagueId, jsonToString(draftOrder)});
    (void)insertState;
    if (!resultOk(updateState.get(), PGRES_COMMAND_OK) || std::string{PQcmdTuples(updateState.get())} == "0") return std::nullopt;
    return dbGetDraftState(accountEmail, leagueId);
}''')

start_db = '''std::optional<Json::Value> dbStartDraft(const std::string &accountEmail,
                                          const std::string &leagueId) {
    if (!dbIsCommissioner(accountEmail, leagueId)) return std::nullopt;
    auto conn = connectToDb();
    if (!conn || !dbDraftLobbyOpen(conn.get(), leagueId)) return std::nullopt;
    auto insertState = execParams(conn.get(),
                                  "INSERT INTO draft_states (league_id, status, current_pick, draft_order, pick_deadline, started_at) "
                                  "VALUES ($1, 'not_started', 1, ARRAY(SELECT email FROM league_members WHERE league_id = $1 AND status = 'active' ORDER BY role, created_at), NULL, NULL) "
                                  "ON CONFLICT (league_id) DO NOTHING",
                                  {leagueId});
    (void)insertState;
    auto current = execParams(conn.get(),
                              "SELECT status FROM draft_states WHERE league_id = $1",
                              {leagueId});
    if (!resultOk(current.get(), PGRES_TUPLES_OK) || PQntuples(current.get()) == 0) return std::nullopt;
    const auto currentStatus = cell(current.get(), 0, 0);
    if (currentStatus == "open") return dbGetDraftState(accountEmail, leagueId);
    if (currentStatus != "not_started") return std::nullopt;
    auto order = draftOrderForLeague(conn.get(), leagueId);
    if (!draftOrderMatchesMembers(conn.get(), leagueId, order)) {
        order = activeDraftOrderForLeague(conn.get(), leagueId);
    }
    if (order.size() < 2 || !draftOrderMatchesMembers(conn.get(), leagueId, order)) return std::nullopt;
    auto picks = execParams(conn.get(),
                            "SELECT COUNT(*) FROM draft_picks WHERE league_id = $1",
                            {leagueId});
    if (!resultOk(picks.get(), PGRES_TUPLES_OK) || cellInt(picks.get(), 0, 0, 0) > 0) return std::nullopt;
    auto update = execParams(conn.get(),
                             "UPDATE draft_states SET status = 'open', current_pick = 1, "
                             "draft_order = ARRAY(SELECT jsonb_array_elements_text($2::jsonb)), "
                             "started_at = NOW(), pick_deadline = NOW() + (pick_clock_seconds * INTERVAL '1 second'), "
                             "updated_at = NOW() WHERE league_id = $1 AND status = 'not_started'",
                             {leagueId, jsonToString(order)});
    if (!resultOk(update.get(), PGRES_COMMAND_OK) || std::string{PQcmdTuples(update.get())} == "0") return std::nullopt;
    return dbGetDraftState(accountEmail, leagueId);
}

'''
if "std::optional<Json::Value> dbStartDraft" not in handler:
    marker = "std::optional<Json::Value> dbSaveDraftQueue("
    index = handler.find(marker)
    if index < 0:
        raise RuntimeError("dbSaveDraftQueue insertion marker missing")
    handler = handler[:index] + start_db + handler[index:]

handler = replace_function(handler, "std::optional<Json::Value> dbMakeDraftPick(const std::string &accountEmail,", '''std::optional<Json::Value> dbMakeDraftPick(const std::string &accountEmail,
                                           const std::string &leagueId,
                                           const Json::Value &player) {
    if (!dbCanAccessLeague(accountEmail, leagueId)) return std::nullopt;
    auto conn = connectToDb();
    if (!conn || !dbDraftLobbyOpen(conn.get(), leagueId)) return std::nullopt;
    auto state = execParams(conn.get(),
                            "SELECT status FROM draft_states WHERE league_id = $1",
                            {leagueId});
    if (!resultOk(state.get(), PGRES_TUPLES_OK) || PQntuples(state.get()) == 0) return std::nullopt;
    const auto currentStatus = cell(state.get(), 0, 0);
    if (currentStatus != "open") return std::nullopt;
    const auto normalized = normalizePlayerJson(player);
    if (jsonString(normalized, "id").empty()) return std::nullopt;
    auto order = draftOrderForLeague(conn.get(), leagueId);
    const auto draftType = draftTypeForLeague(conn.get(), leagueId);
    auto pickNumberResult = execParams(conn.get(),
                                       "SELECT COALESCE(MAX(pick_number), 0) + 1 FROM draft_picks WHERE league_id = $1",
                                       {leagueId});
    const auto pickNumber = resultOk(pickNumberResult.get(), PGRES_TUPLES_OK) && PQntuples(pickNumberResult.get()) > 0
                                ? cellInt(pickNumberResult.get(), 0, 0, 1)
                                : 1;
    const auto expectedManager = cff::league_schedule::currentDraftManager(order, pickNumber, draftType);
    if (!expectedManager.empty() && canonicalEmail(expectedManager) != canonicalEmail(accountEmail)) return std::nullopt;
    auto pickResult = execParams(conn.get(),
                                 "INSERT INTO draft_picks (id, league_id, manager_email, pick_number, player_id, player_snapshot) "
                                 "VALUES ($1, $2, $3, $4::int, $5, $6::jsonb)",
                                 {timestampId("pick"), leagueId, accountEmail, std::to_string(pickNumber), jsonString(normalized, "id"), jsonToString(normalized)});
    if (!resultOk(pickResult.get(), PGRES_COMMAND_OK)) return std::nullopt;
    const auto slot = dbAssignRosterSlot(conn.get(), leagueId, accountEmail, normalized);
    if (!slot) return std::nullopt;
    auto rosterResult = execParams(conn.get(),
                                   "INSERT INTO rosters (league_id, manager_email, player_id, player_snapshot, roster_slot, acquired_via) "
                                   "VALUES ($1, $2, $3, $4::jsonb, $5, 'draft') "
                                   "ON CONFLICT (league_id, manager_email, player_id) DO UPDATE SET player_snapshot = EXCLUDED.player_snapshot, roster_slot = EXCLUDED.roster_slot, acquired_via = 'draft'",
                                   {leagueId, accountEmail, jsonString(normalized, "id"), jsonToString(normalized), *slot});
    auto queueResult = execParams(conn.get(),
                                  "UPDATE draft_queues SET queue = COALESCE((SELECT jsonb_agg(item) FROM jsonb_array_elements(queue) item WHERE item->>'id' <> $3), '[]'::jsonb), updated_at = NOW() "
                                  "WHERE league_id = $1 AND manager_email = $2",
                                  {leagueId, accountEmail, jsonString(normalized, "id")});
    auto stateResult = execParams(conn.get(),
                                  "UPDATE draft_states SET current_pick = $2::int + 1, "
                                  "status = CASE WHEN $3 = 'true' THEN 'complete' ELSE status END, "
                                  "pick_deadline = CASE WHEN $3 = 'true' THEN NULL ELSE NOW() + (pick_clock_seconds * INTERVAL '1 second') END, "
                                  "updated_at = NOW() WHERE league_id = $1 AND status = 'open'",
                                  {leagueId, std::to_string(pickNumber), dbDraftComplete(conn.get(), leagueId) ? "true" : "false"});
    (void)queueResult;
    (void)stateResult;
    if (!resultOk(rosterResult.get(), PGRES_COMMAND_OK)) return std::nullopt;
    dbAddTransaction(conn.get(), leagueId, "Draft Pick", "Drafted " + jsonString(normalized, "name"), accountEmail, normalized);
    return dbGetDraftState(accountEmail, leagueId);
}''')

handler = replace_function(handler, "std::optional<Json::Value> dbResetDraft(const std::string &accountEmail, const std::string &leagueId)", '''std::optional<Json::Value> dbResetDraft(const std::string &accountEmail, const std::string &leagueId) {
    if (!dbIsCommissioner(accountEmail, leagueId)) return std::nullopt;
    auto conn = connectToDb();
    if (!conn) return std::nullopt;
    auto picks = execParams(conn.get(), "DELETE FROM draft_picks WHERE league_id = $1", {leagueId});
    auto rosters = execParams(conn.get(), "DELETE FROM rosters WHERE league_id = $1 AND acquired_via = 'draft'", {leagueId});
    auto insertState = execParams(conn.get(),
                                  "INSERT INTO draft_states (league_id, status, current_pick, draft_order, pick_deadline, started_at) "
                                  "VALUES ($1, 'not_started', 1, ARRAY(SELECT email FROM league_members WHERE league_id = $1 AND status = 'active' ORDER BY role, created_at), NULL, NULL) "
                                  "ON CONFLICT (league_id) DO NOTHING",
                                  {leagueId});
    auto state = execParams(conn.get(),
                            "UPDATE draft_states SET current_pick = 1, status = 'not_started', "
                            "pick_deadline = NULL, started_at = NULL, updated_at = NOW() WHERE league_id = $1",
                            {leagueId});
    (void)picks;
    (void)rosters;
    (void)insertState;
    if (!resultOk(state.get(), PGRES_COMMAND_OK)) return std::nullopt;
    return dbGetDraftState(accountEmail, leagueId);
}''')

handler = replace_function(handler, "void handleGetDraftState(const drogon::HttpRequestPtr&", '''void handleGetDraftState(const drogon::HttpRequestPtr&,
                         std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                         const std::string &accountEmail,
                         const std::string &leagueId) {
#ifdef CFF_HAS_POSTGRES
    if (dbConfigured()) {
        auto state = dbGetDraftState(accountEmail, leagueId);
        if (!state) {
            sendError(callback, drogon::k404NotFound, "League not found");
            return;
        }
        callback(jsonResponse(*state, drogon::k200OK));
        return;
    }
#endif

    std::lock_guard<std::mutex> lock(storeMutex);
    if (!ensureLeagueAccess(callback, accountEmail, leagueId)) return;
    callback(jsonResponse(localDraftPayloadLocked(accountEmail, leagueId), drogon::k200OK));
}''')

handler = replace_function(handler, "void handleSaveDraftQueue(const drogon::HttpRequestPtr &req,", '''void handleSaveDraftQueue(const drogon::HttpRequestPtr &req,
                          std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                          const std::string &accountEmail,
                          const std::string &leagueId) {
    const auto body = req->getJsonObject();
    const auto queue = body && body->isMember("queue") && (*body)["queue"].isArray() ? (*body)["queue"] : Json::Value{Json::arrayValue};
#ifdef CFF_HAS_POSTGRES
    if (dbConfigured()) {
        auto state = dbSaveDraftQueue(accountEmail, leagueId, queue);
        if (!state) {
            sendError(callback, drogon::k404NotFound, "League not found");
            return;
        }
        callback(jsonResponse(*state, drogon::k200OK));
        return;
    }
#endif

    std::lock_guard<std::mutex> lock(storeMutex);
    if (!ensureLeagueAccess(callback, accountEmail, leagueId)) return;
    draftQueuesByLeagueManager[leagueId + ":" + accountEmail] = queue;
    callback(jsonResponse(localDraftPayloadLocked(accountEmail, leagueId), drogon::k200OK));
}''')

handler = replace_function(handler, "void handleSaveDraftOrder(const drogon::HttpRequestPtr &req,", '''void handleSaveDraftOrder(const drogon::HttpRequestPtr &req,
                          std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                          const std::string &accountEmail,
                          const std::string &leagueId) {
    const auto body = req->getJsonObject();
    const auto order = body && body->isMember("draftOrder") && (*body)["draftOrder"].isArray()
                           ? (*body)["draftOrder"]
                           : Json::Value{Json::arrayValue};
#ifdef CFF_HAS_POSTGRES
    if (dbConfigured()) {
        auto state = dbSaveDraftOrder(accountEmail, leagueId, order);
        if (!state) {
            sendError(callback, drogon::k409Conflict, "Unable to save draft order");
            return;
        }
        callback(jsonResponse(*state, drogon::k200OK));
        return;
    }
#endif

    std::lock_guard<std::mutex> lock(storeMutex);
    if (!ensureCommissionerAccess(callback, accountEmail, leagueId)) return;
    auto &state = localDraftStateLocked(leagueId);
    if (state["status"].asString() != "not_started"
        || !arrayForLeague(draftPicksByLeague, leagueId).empty()
        || !localDraftOrderMatchesActiveMembers(leagueId, order)) {
        sendError(callback, drogon::k409Conflict, "Unable to save draft order");
        return;
    }
    state["draftOrder"] = order;
    state["currentPick"] = 1;
    state["pickDeadline"] = "";
    state["startedAt"] = "";
    callback(jsonResponse(localDraftPayloadLocked(accountEmail, leagueId), drogon::k200OK));
}''')

start_handler = '''void handleStartDraft(const drogon::HttpRequestPtr&,
                      std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                      const std::string &accountEmail,
                      const std::string &leagueId) {
#ifdef CFF_HAS_POSTGRES
    if (dbConfigured()) {
        auto state = dbStartDraft(accountEmail, leagueId);
        if (!state) {
            sendError(callback, drogon::k403Forbidden, "Commissioner access and an open lobby with at least two active managers are required");
            return;
        }
        callback(jsonResponse(*state, drogon::k200OK));
        return;
    }
#endif

    std::lock_guard<std::mutex> lock(storeMutex);
    if (!ensureCommissionerAccess(callback, accountEmail, leagueId)) return;
    auto &state = localDraftStateLocked(leagueId);
    if (!state["lobbyOpen"].asBool()) {
        sendError(callback, drogon::k403Forbidden, "Draft lobby is not open");
        return;
    }
    if (state["status"].asString() == "open") {
        callback(jsonResponse(localDraftPayloadLocked(accountEmail, leagueId), drogon::k200OK));
        return;
    }
    auto order = state["draftOrder"];
    if (!localDraftOrderMatchesActiveMembers(leagueId, order)) {
        order = activeDraftOrderLocked(leagueId);
    }
    if (order.size() < 2 || !localDraftOrderMatchesActiveMembers(leagueId, order)
        || !arrayForLeague(draftPicksByLeague, leagueId).empty()) {
        sendError(callback, drogon::k409Conflict, "At least two active managers and a valid draft order are required");
        return;
    }
    state["status"] = "open";
    state["currentPick"] = 1;
    state["draftOrder"] = order;
    state["startedAt"] = draftIsoNow();
    state["pickDeadline"] = draftDeadlineFromNow(state.get("pickClockSeconds", 90).asInt());
    callback(jsonResponse(localDraftPayloadLocked(accountEmail, leagueId), drogon::k200OK));
}

'''
if "void handleStartDraft" not in handler:
    marker = "void handleMakeDraftPick("
    index = handler.find(marker)
    if index < 0:
        raise RuntimeError("handleMakeDraftPick insertion marker missing")
    handler = handler[:index] + start_handler + handler[index:]

handler = replace_function(handler, "void handleMakeDraftPick(const drogon::HttpRequestPtr &req,", '''void handleMakeDraftPick(const drogon::HttpRequestPtr &req,
                         std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                         const std::string &accountEmail,
                         const std::string &leagueId) {
    const auto body = req->getJsonObject();
    if (!body || !body->isObject() || !body->isMember("player")) {
        sendError(callback, drogon::k400BadRequest, "player is required");
        return;
    }
#ifdef CFF_HAS_POSTGRES
    if (dbConfigured()) {
        auto state = dbMakeDraftPick(accountEmail, leagueId, (*body)["player"]);
        if (!state) {
            sendError(callback, drogon::k409Conflict, "Unable to make draft pick");
            return;
        }
        callback(jsonResponse(*state, drogon::k201Created));
        return;
    }
#endif

    std::lock_guard<std::mutex> lock(storeMutex);
    if (!ensureLeagueAccess(callback, accountEmail, leagueId)) return;
    auto &state = localDraftStateLocked(leagueId);
    if (!state["lobbyOpen"].asBool() || state["status"].asString() != "open") {
        sendError(callback, drogon::k409Conflict, "Draft has not started");
        return;
    }
    const auto expectedManager = cff::league_schedule::currentDraftManager(
        state["draftOrder"], state.get("currentPick", 1).asInt(), state.get("draftType", "snake").asString());
    if (!expectedManager.empty() && canonicalEmail(expectedManager) != canonicalEmail(accountEmail)) {
        sendError(callback, drogon::k409Conflict, "It is not this manager's turn");
        return;
    }
    auto player = normalizePlayerJson((*body)["player"]);
    const auto playerId = jsonString(player, "id");
    if (playerId.empty()) {
        sendError(callback, drogon::k400BadRequest, "player id is required");
        return;
    }
    auto &picks = arrayForLeague(draftPicksByLeague, leagueId);
    for (const auto &existing : picks) {
        if (jsonString(existing["player"], "id") == playerId) {
            sendError(callback, drogon::k409Conflict, "Player has already been drafted");
            return;
        }
    }
    Json::Value pick;
    pick["id"] = timestampId("pick");
    pick["managerEmail"] = accountEmail;
    pick["pickNumber"] = static_cast<int>(picks.size()) + 1;
    pick["player"] = player;
    pick["createdAt"] = draftIsoNow();
    picks.append(pick);
    auto &roster = arrayForLeague(rostersByLeague, leagueId);
    if (indexOfPlayer(roster, playerId) < 0) roster.append(player);
    auto &queue = arrayForLeague(draftQueuesByLeagueManager, leagueId + ":" + accountEmail);
    removePlayer(queue, playerId);
    addTransactionLocked(leagueId, "Draft Pick", "Drafted " + jsonString(player, "name"), accountEmail);
    state["currentPick"] = static_cast<int>(picks.size()) + 1;
    state["pickDeadline"] = draftDeadlineFromNow(state.get("pickClockSeconds", 90).asInt());
    callback(jsonResponse(localDraftPayloadLocked(accountEmail, leagueId), drogon::k201Created));
}''')

handler = replace_function(handler, "void handleResetDraft(const drogon::HttpRequestPtr&", '''void handleResetDraft(const drogon::HttpRequestPtr&,
                      std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                      const std::string &accountEmail,
                      const std::string &leagueId) {
#ifdef CFF_HAS_POSTGRES
    if (dbConfigured()) {
        auto state = dbResetDraft(accountEmail, leagueId);
        if (!state) {
            sendError(callback, drogon::k403Forbidden, "Commissioner access required");
            return;
        }
        callback(jsonResponse(*state, drogon::k200OK));
        return;
    }
#endif

    std::lock_guard<std::mutex> lock(storeMutex);
    if (!ensureCommissionerAccess(callback, accountEmail, leagueId)) return;
    draftPicksByLeague[leagueId] = Json::Value{Json::arrayValue};
    rostersByLeague[leagueId] = Json::Value{Json::arrayValue};
    auto &state = localDraftStateLocked(leagueId);
    state["status"] = "not_started";
    state["currentPick"] = 1;
    state["pickDeadline"] = "";
    state["startedAt"] = "";
    callback(jsonResponse(localDraftPayloadLocked(accountEmail, leagueId), drogon::k200OK));
}''')

handler = replace_function(handler, "void handleUndoDraftPick(const drogon::HttpRequestPtr&", '''void handleUndoDraftPick(const drogon::HttpRequestPtr&,
                         std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                         const std::string &accountEmail,
                         const std::string &leagueId) {
#ifdef CFF_HAS_POSTGRES
    if (dbConfigured()) {
        auto state = dbUndoDraftPick(accountEmail, leagueId);
        if (!state) {
            sendError(callback, drogon::k403Forbidden, "Commissioner access required");
            return;
        }
        callback(jsonResponse(*state, drogon::k200OK));
        return;
    }
#endif

    std::lock_guard<std::mutex> lock(storeMutex);
    if (!ensureCommissionerAccess(callback, accountEmail, leagueId)) return;
    auto &picks = arrayForLeague(draftPicksByLeague, leagueId);
    auto &state = localDraftStateLocked(leagueId);
    if (!picks.empty()) {
        Json::Value removed;
        picks.removeIndex(static_cast<Json::ArrayIndex>(picks.size() - 1), &removed);
        const auto managerEmail = jsonString(removed, "managerEmail", accountEmail);
        const auto player = removed["player"];
        const auto playerId = jsonString(player, "id");
        auto &roster = arrayForLeague(rostersByLeague, leagueId);
        removePlayer(roster, playerId);
        auto &queue = arrayForLeague(draftQueuesByLeagueManager, leagueId + ":" + managerEmail);
        if (indexOfPlayer(queue, playerId) < 0) queue.append(player);
        state["status"] = "open";
        state["currentPick"] = static_cast<int>(picks.size()) + 1;
        state["pickDeadline"] = draftDeadlineFromNow(state.get("pickClockSeconds", 90).asInt());
    }
    callback(jsonResponse(localDraftPayloadLocked(accountEmail, leagueId), drogon::k200OK));
}''')

HANDLER.write_text(handler, encoding="utf-8")

header = HEADER.read_text(encoding="utf-8")
header_anchor = '''void handleSaveDraftOrder(const drogon::HttpRequestPtr &req,
                          std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                          const std::string &accountEmail,
                          const std::string &leagueId);

'''
header_insert = header_anchor + '''void handleStartDraft(const drogon::HttpRequestPtr &req,
                      std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                      const std::string &accountEmail,
                      const std::string &leagueId);

'''
if "void handleStartDraft" not in header:
    header = replace_once(header, header_anchor, header_insert, "draft start declaration anchor")
HEADER.write_text(header, encoding="utf-8")

routes = ROUTES.read_text(encoding="utf-8")
route_anchor = '''        .registerHandler("/api/leagues/{1}/draft/picks",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
'''
start_route = '''        .registerHandler("/api/leagues/{1}/draft/start",
                         [jwtSecret](const drogon::HttpRequestPtr& req,
                                     std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                                     const std::string &leagueId) {
                             std::string accountEmail;
                             if (!cff::http::requireAccount(req, callback, jwtSecret, accountEmail)) {
                                 return;
                             }
                             cff::handlers::handleStartDraft(req, std::move(callback), accountEmail, leagueId);
                         },
                         {drogon::Post})
'''
if '"/api/leagues/{1}/draft/start"' not in routes:
    routes = replace_once(routes, route_anchor, start_route + route_anchor, "draft start route anchor")
preflight_anchor = '        .registerHandler("/api/leagues/{1}/draft/picks", preflightOneParamHandler, {drogon::Options})\n'
preflight_insert = '        .registerHandler("/api/leagues/{1}/draft/start", preflightOneParamHandler, {drogon::Options})\n' + preflight_anchor
if routes.count('"/api/leagues/{1}/draft/start"') == 1:
    routes = replace_once(routes, preflight_anchor, preflight_insert, "draft start preflight anchor")
ROUTES.write_text(routes, encoding="utf-8")

state = STATE.read_text(encoding="utf-8")
state = state.replace("      status: 'open',\n      currentPick: 1,", "      status: 'not_started',\n      currentPick: 1,")
state = state.replace(": { status: 'open', currentPick: 1, draftOrder: [], draftType: 'snake'", ": { status: 'not_started', currentPick: 1, draftOrder: [], draftType: 'snake'")
state = replace_once(state, "    status: meta.status || 'open',", "    status: meta.status || 'not_started',", "draft meta status default")
state = replace_once(
    state,
    "    pickDeadline: meta.pickDeadline || ''\n",
    "    pickDeadline: meta.pickDeadline || '',\n    startedAt: meta.startedAt || '',\n    lobbyOpen: typeof meta.lobbyOpen === 'boolean' ? meta.lobbyOpen : Boolean(league.draftLobbyOpen)\n",
    "draft meta timing fields",
)
state = replace_once(
    state,
    "  if (meta.status === 'complete') return roster;",
    "  if (meta.status !== 'open') return roster;",
    "local draft live-state guard",
)
apply_old = '''function applyDraftState(state = {}) {
  if (Array.isArray(state.queue)) {
    setQueue(state.queue.map(normalizePlayer));
  }
  if (Array.isArray(state.picks)) {
    saveDraftPicks(state.picks);
  }
  saveDraftMeta(state);
}
'''
apply_new = '''function applyDraftState(state = {}) {
  if (Array.isArray(state.queue)) {
    setQueue(state.queue.map(normalizePlayer));
  }
  if (Array.isArray(state.picks)) {
    saveDraftPicks(state.picks);
  }
  const league = getLeagueState();
  if (league && typeof state.lobbyOpen === 'boolean') {
    saveLeagueForAccount({
      ...league,
      draftLobbyOpen: state.lobbyOpen,
      draftLobbyStartedAt: league.draftLobbyStartedAt || ''
    });
  }
  saveDraftMeta(state);
}
'''
state = replace_once(state, apply_old, apply_new, "authoritative draft state application")

local_start_anchor = '''function saveDraftOrder(draftOrder = []) {
  const meta = getDraftMeta();
  const currentPick = Number(meta.currentPick || 1);
  saveDraftMeta({
    ...meta,
    status: meta.status || 'open',
    currentPick,
    draftOrder,
    currentManager: draftManagerForPick(draftOrder, currentPick, meta.draftType || getLeagueState()?.draftType || 'snake'),
    pickDeadline: meta.pickDeadline || new Date(Date.now() + Number(meta.pickClockSeconds || 90) * 1000).toISOString()
  });
}
'''
local_start_replacement = '''function saveDraftOrder(draftOrder = []) {
  const meta = getDraftMeta();
  const currentPick = Number(meta.currentPick || 1);
  saveDraftMeta({
    ...meta,
    status: meta.status || 'not_started',
    currentPick,
    draftOrder,
    currentManager: draftManagerForPick(draftOrder, currentPick, meta.draftType || getLeagueState()?.draftType || 'snake'),
    pickDeadline: meta.status === 'open'
      ? meta.pickDeadline || new Date(Date.now() + Number(meta.pickClockSeconds || 90) * 1000).toISOString()
      : ''
  });
}

function startDraft() {
  const league = getLeagueState();
  const activeManagers = (league?.members || []).filter((member) => String(member.status || '').toLowerCase() === 'active');
  if (!isCurrentCommissioner(league) || !league?.draftLobbyOpen || activeManagers.length < 2) return null;
  const meta = getDraftMeta();
  if (meta.status === 'open') return meta;
  const draftOrder = Array.isArray(meta.draftOrder) && meta.draftOrder.length
    ? meta.draftOrder
    : activeManagers.map((member) => member.email).filter(Boolean);
  const startedAt = new Date().toISOString();
  const next = {
    ...meta,
    status: 'open',
    currentPick: 1,
    draftOrder,
    currentManager: draftManagerForPick(draftOrder, 1, meta.draftType || league.draftType || 'snake'),
    startedAt,
    pickDeadline: new Date(Date.now() + Number(meta.pickClockSeconds || 90) * 1000).toISOString(),
    lobbyOpen: true
  };
  saveDraftMeta(next);
  return next;
}
'''
state = replace_once(state, local_start_anchor, local_start_replacement, "local draft start insertion")

api_start_anchor = '''async function saveDraftOrderApi(draftOrder = []) {
  const league = getLeagueState();
  if (!getAuthState()?.token || isLocalDemoSession()) {
    saveDraftOrder(draftOrder);
    return null;
  }
  if (!league?.id) throw new Error('No server league selected');
  const state = await apiRequest(`/leagues/${encodeURIComponent(league.id)}/draft/order`, {
    method: 'PUT',
    body: JSON.stringify({ draftOrder })
  });
  applyDraftState(state);
  return state;
}
'''
api_start_replacement = api_start_anchor + '''
async function startDraftApi() {
  const league = getLeagueState();
  if (!getAuthState()?.token || isLocalDemoSession()) {
    return startDraft();
  }
  if (!league?.id) throw new Error('No server league selected');
  const state = await apiRequest(`/leagues/${encodeURIComponent(league.id)}/draft/start`, {
    method: 'POST'
  });
  applyDraftState(state);
  return state;
}
'''
if "async function startDraftApi" not in state:
    state = replace_once(state, api_start_anchor, api_start_replacement, "draft start API insertion")
state = replace_once(
    state,
    "function draftClockRemaining(meta = getDraftMeta()) {\n  if (!meta.pickDeadline)",
    "function draftClockRemaining(meta = getDraftMeta()) {\n  if (meta.status !== 'open') return 0;\n  if (!meta.pickDeadline)",
    "waiting draft clock guard",
)
STATE.write_text(state, encoding="utf-8")

draft = DRAFT.read_text(encoding="utf-8")
dom_anchor = "const draftRoomContent = document.getElementById('draft-room-content');\n"
dom_insert = dom_anchor + "const draftLobbyBadge = document.getElementById('draft-lobby-badge');\nconst draftLobbyCopy = document.getElementById('draft-lobby-copy');\nconst draftStartBtn = document.getElementById('draft-start');\nconst draftLobbyMembers = document.getElementById('draft-lobby-members');\n"
if "const draftStartBtn" not in draft:
    draft = replace_once(draft, dom_anchor, dom_insert, "draft lobby DOM anchor")
draft = replace_once(
    draft,
    "let autoPickInFlight = false;\n",
    "let autoPickInFlight = false;\nlet draftSyncTimer = null;\nlet draftRefreshInFlight = false;\n",
    "draft sync state anchor",
)
access_return = "  const orderLocked = getDraftPicks().length > 0 || getDraftMeta().status === 'complete';"
draft = replace_once(
    draft,
    access_return,
    "  const orderLocked = getDraftPicks().length > 0 || getDraftMeta().status !== 'not_started';",
    "draft order lock state",
)
render_access_end = "  return canEnter;\n}\n\nfunction renderLeagueHeader()"
lobby_render = '''  return canEnter;
}

function renderDraftLobbyState() {
  const league = getLeagueState();
  const meta = getDraftMeta();
  const commissioner = isCurrentCommissioner(league);
  const activeManagers = (league?.members || []).filter((member) => String(member.status || '').toLowerCase() === 'active');
  const waiting = meta.status === 'not_started';
  const live = meta.status === 'open';
  if (draftLobbyMembers) {
    draftLobbyMembers.textContent = `${activeManagers.length} active manager${activeManagers.length === 1 ? '' : 's'}`;
  }
  if (draftLobbyBadge) {
    draftLobbyBadge.textContent = meta.status === 'complete' ? 'Complete' : live ? 'Live' : 'Lobby';
  }
  if (draftLobbyCopy) {
    if (!league?.draftLobbyOpen) {
      draftLobbyCopy.textContent = commissioner
        ? 'Open the lobby from league settings before starting the draft.'
        : 'Waiting for the commissioner to open this draft lobby.';
    } else if (meta.status === 'complete') {
      draftLobbyCopy.textContent = 'The draft is complete. The commissioner can reset it for another test.';
    } else if (live) {
      draftLobbyCopy.textContent = `Draft started${meta.startedAt ? ` ${new Date(meta.startedAt).toLocaleString()}` : ''}. Picks refresh automatically for every manager.`;
    } else if (commissioner && activeManagers.length < 2) {
      draftLobbyCopy.textContent = 'At least two active managers are required before the draft can start.';
    } else if (commissioner) {
      draftLobbyCopy.textContent = 'Managers may enter now. Start the draft when everyone is ready.';
    } else {
      draftLobbyCopy.textContent = 'You are in the lobby. Waiting for the commissioner to start the draft.';
    }
  }
  if (draftStartBtn) {
    draftStartBtn.hidden = !commissioner || !waiting;
    draftStartBtn.disabled = !league?.draftLobbyOpen || activeManagers.length < 2;
  }
}

function renderLeagueHeader()'''
if "function renderDraftLobbyState" not in draft:
    draft = replace_once(draft, render_access_end, lobby_render, "draft lobby rendering insertion")

# Only active managers belong in the draft order.
draft = draft.replace(
    ".filter((member) => String(member.status || '').toLowerCase() !== 'removed')",
    ".filter((member) => String(member.status || '').toLowerCase() === 'active')",
)

queue_old = "  const complete = meta.status === 'complete';\n"
queue_new = "  const complete = meta.status === 'complete';\n  const live = meta.status === 'open';\n"
draft = replace_once(draft, queue_old, queue_new, "draft queue live-state anchor")
draft = replace_once(
    draft,
    "${myTurn && !complete ? '' : 'disabled'}>${complete ? 'Complete' : myTurn ? 'Draft' : 'Waiting'}",
    "${live && myTurn && !complete ? '' : 'disabled'}>${complete ? 'Complete' : !live ? 'Not started' : myTurn ? 'Draft' : 'Waiting'}",
    "draft queue button state",
)

render_picks_anchor = '''  if (draftCurrentPick) {
    draftCurrentPick.textContent = meta.status === 'complete' ? 'Complete' : `Pick ${currentPick}`;
  }
  if (draftCurrentManager) draftCurrentManager.textContent = meta.status === 'complete' ? 'Draft complete' : managerDisplayName(manager) || 'Manager TBD';
  if (draftStatus) draftStatus.textContent = meta.status === 'complete' ? 'Complete' : isMyDraftTurn(meta) ? 'Your pick' : 'Waiting';
  if (draftRoundLabel) draftRoundLabel.textContent = meta.status === 'complete' ? 'Complete' : `Round ${round}`;
  if (draftNextPickLabel) draftNextPickLabel.textContent = meta.status === 'complete' ? 'Done' : `Pick ${currentPick}`;
'''
render_picks_new = '''  const waiting = meta.status === 'not_started';
  if (draftCurrentPick) {
    draftCurrentPick.textContent = meta.status === 'complete' ? 'Complete' : waiting ? 'Waiting' : `Pick ${currentPick}`;
  }
  if (draftCurrentManager) draftCurrentManager.textContent = meta.status === 'complete'
    ? 'Draft complete'
    : waiting
      ? 'Commissioner starts draft'
      : managerDisplayName(manager) || 'Manager TBD';
  if (draftStatus) draftStatus.textContent = meta.status === 'complete'
    ? 'Complete'
    : waiting
      ? 'Lobby'
      : isMyDraftTurn(meta) ? 'Your pick' : 'Waiting';
  if (draftRoundLabel) draftRoundLabel.textContent = meta.status === 'complete' ? 'Complete' : waiting ? 'Lobby' : `Round ${round}`;
  if (draftNextPickLabel) draftNextPickLabel.textContent = meta.status === 'complete' ? 'Done' : waiting ? 'Not started' : `Pick ${currentPick}`;
'''
draft = replace_once(draft, render_picks_anchor, render_picks_new, "draft waiting status rendering")

draft = replace_once(
    draft,
    "  if (draftClock) {\n    draftClock.textContent = meta.status === 'complete' ? 'Done' : `${remaining}s`;\n  }",
    "  if (draftClock) {\n    draftClock.textContent = meta.status === 'complete' ? 'Done' : meta.status !== 'open' ? 'Waiting' : `${remaining}s`;\n  }",
    "draft clock waiting display",
)
draft = replace_once(
    draft,
    "  if (autoPickInFlight || meta.status === 'complete' || !isMyDraftTurn(meta) || draftClockRemaining(meta) > 0) return;",
    "  if (autoPickInFlight || meta.status !== 'open' || !isMyDraftTurn(meta) || draftClockRemaining(meta) > 0) return;",
    "auto-pick live-state guard",
)
refresh_old = '''async function refreshDraftFromApi() {
  if (!canEnterDraftRoom()) return;
  if (!getAuthState()?.token) return;
  try {
    await syncLeaguesFromApi();
    await syncActiveLeagueCollectionsFromApi();
    await syncDraftFromApi();
  } catch {
    // Keep local draft controls responsive when the API is offline.
  }
}
'''
refresh_new = '''async function refreshDraftFromApi() {
  if (!getAuthState()?.token) return;
  if (draftRefreshInFlight) return;
  draftRefreshInFlight = true;
  try {
    await syncLeaguesFromApi();
    await syncActiveLeagueCollectionsFromApi();
    await syncDraftFromApi();
  } catch {
    // Keep the last authoritative draft snapshot visible during an outage.
  } finally {
    draftRefreshInFlight = false;
  }
}

function startDraftSyncPolling() {
  if (draftSyncTimer) clearInterval(draftSyncTimer);
  draftSyncTimer = setInterval(async () => {
    if (document.visibilityState !== 'visible' || !getAuthState()?.token) return;
    await refreshDraftFromApi();
    renderAll();
  }, 2000);
}
'''
draft = replace_once(draft, refresh_old, refresh_new, "authoritative draft refresh and polling")
draft = replace_once(
    draft,
    "  renderLeagueHeader();\n",
    "  renderLeagueHeader();\n  renderDraftLobbyState();\n",
    "draft lobby render call",
)
start_listener_anchor = "refreshDraftBtn?.addEventListener('click', async () => {\n"
start_listener = '''draftStartBtn?.addEventListener('click', async () => {
  if (!isCurrentCommissioner()) return;
  draftStartBtn.disabled = true;
  try {
    await startDraftApi();
    await refreshDraftFromApi();
  } catch (error) {
    if (draftLobbyCopy) draftLobbyCopy.textContent = mutationErrorMessage(error, 'Could not start the draft.');
  }
  renderAll();
});

'''
if "draftStartBtn?.addEventListener" not in draft:
    draft = replace_once(draft, start_listener_anchor, start_listener + start_listener_anchor, "draft start listener anchor")
draft = replace_once(
    draft,
    "  startDraftTimer();\n}",
    "  startDraftTimer();\n  startDraftSyncPolling();\n}",
    "draft polling initialization",
)
DRAFT.write_text(draft, encoding="utf-8")

draft_html = DRAFT_HTML.read_text(encoding="utf-8")
lobby_html_anchor = '''  <div id="draft-room-content">
  <header class="hero hero--subtle">
'''
lobby_html = '''  <div id="draft-room-content">
  <main class="layout" style="max-width: 1080px; padding-bottom: 0">
    <section class="card card--accent">
      <div class="card__header">
        <div>
          <h2>Draft lobby</h2>
          <div class="muted small" id="draft-lobby-copy">Waiting for the latest league draft state.</div>
        </div>
        <span class="pill" id="draft-lobby-badge">Lobby</span>
      </div>
      <div class="row">
        <div>
          <strong id="draft-lobby-members">0 active managers</strong>
          <div class="muted">Everyone in the room sees the same draft order, clock, and picks.</div>
        </div>
        <button class="button button--primary" id="draft-start" type="button">Start draft</button>
      </div>
    </section>
  </main>
  <header class="hero hero--subtle">
'''
if "id=\"draft-start\"" not in draft_html:
    draft_html = replace_once(draft_html, lobby_html_anchor, lobby_html, "draft lobby markup anchor")
DRAFT_HTML.write_text(draft_html, encoding="utf-8")

league = LEAGUE.read_text(encoding="utf-8")
league_dom_anchor = "const draftLobbyLink = document.getElementById('draft-lobby-link');\n"
league_dom_insert = league_dom_anchor + "const draftLobbyOverviewStatus = document.getElementById('draft-lobby-overview-status');\nconst draftLobbyOverviewLink = document.getElementById('draft-lobby-overview-link');\nconst draftLobbyOverviewBadge = document.getElementById('draft-lobby-overview-badge');\nconst leagueDraftTabLink = document.getElementById('league-draft-tab-link');\nconst teamDraftLink = document.getElementById('team-draft-link');\n"
if "const draftLobbyOverviewStatus" not in league:
    league = replace_once(league, league_dom_anchor, league_dom_insert, "league lobby overview DOM anchor")
old_lobby_function = '''function renderLobbyStatus(leagueState) {
  if (!draftLobbyStatus) return;
  if (!leagueState) {
    draftLobbyStatus.textContent = 'Create a league before opening the draft lobby.';
    return;
  }
  if (!leagueState.draftLobbyOpen) {
    draftLobbyStatus.textContent = 'Not opened yet.';
    return;
  }
  const opened = leagueState.draftLobbyStartedAt
    ? `Opened ${new Date(leagueState.draftLobbyStartedAt).toLocaleString()}.`
    : 'Open now.';
  draftLobbyStatus.textContent = `${opened} Managers can enter the draft room.`;
  if (draftLobbyLink) {
    draftLobbyLink.href = `draft.html?league=${encodeURIComponent(leagueState.id)}`;
  }
}
'''
new_lobby_function = '''function renderLobbyStatus(leagueState) {
  const commissioner = isCurrentCommissioner(leagueState);
  const links = [draftLobbyLink, draftLobbyOverviewLink, leagueDraftTabLink, teamDraftLink].filter(Boolean);
  if (!leagueState) {
    if (draftLobbyStatus) draftLobbyStatus.textContent = 'Create a league before opening the draft lobby.';
    if (draftLobbyOverviewStatus) draftLobbyOverviewStatus.textContent = 'No league selected.';
    links.forEach((link) => {
      link.href = 'league.html';
      link.setAttribute('aria-disabled', 'true');
    });
    return;
  }
  const href = `draft.html?league=${encodeURIComponent(leagueState.id)}`;
  links.forEach((link) => {
    link.href = href;
    link.removeAttribute('aria-disabled');
  });
  if (!leagueState.draftLobbyOpen) {
    if (draftLobbyStatus) draftLobbyStatus.textContent = 'Not opened yet.';
    if (draftLobbyOverviewStatus) {
      draftLobbyOverviewStatus.textContent = commissioner
        ? 'Open the lobby when active managers are ready to enter.'
        : 'Waiting for the commissioner to open the room.';
    }
    if (draftLobbyOverviewBadge) draftLobbyOverviewBadge.textContent = 'Closed';
    if (draftLobbyOverviewLink) draftLobbyOverviewLink.hidden = !commissioner;
    if (draftLobbyLink) draftLobbyLink.hidden = !commissioner;
    if (stepLobby) stepLobby.textContent = 'Open lobby';
    return;
  }
  const opened = leagueState.draftLobbyStartedAt
    ? `Opened ${new Date(leagueState.draftLobbyStartedAt).toLocaleString()}.`
    : 'Open now.';
  const message = `${opened} Active managers can enter and wait for the commissioner to start.`;
  if (draftLobbyStatus) draftLobbyStatus.textContent = message;
  if (draftLobbyOverviewStatus) draftLobbyOverviewStatus.textContent = message;
  if (draftLobbyOverviewBadge) draftLobbyOverviewBadge.textContent = 'Open';
  if (draftLobbyOverviewLink) draftLobbyOverviewLink.hidden = false;
  if (draftLobbyLink) draftLobbyLink.hidden = false;
  if (stepLobby) stepLobby.textContent = 'Enter lobby';
}
'''
league = replace_once(league, old_lobby_function, new_lobby_function, "league lobby status rendering")
step_old = '''stepLobby?.addEventListener('click', async () => {
  if (!requireCommissioner()) return;
  const current = getLeagueState();
  if (!current) {
    setSettingsStatus('Create a league before opening the draft lobby.', true);
    return;
  }
  const updated = normalizeLeague({
    ...current,
    draftLobbyOpen: true,
    draftLobbyStartedAt: current.draftLobbyStartedAt || new Date().toISOString()
  });
  try {
    await saveLeagueToApi(updated);
  } catch (error) {
    setSettingsStatus(mutationErrorMessage(error, 'Could not open draft lobby. No local changes were made.'), true);
    renderLeague();
    return;
  }
  renderLeague();
  window.location.href = `draft.html?league=${encodeURIComponent(updated.id)}`;
});
'''
step_new = '''stepLobby?.addEventListener('click', async () => {
  if (!requireCommissioner()) return;
  const current = getLeagueState();
  if (!current) {
    setSettingsStatus('Create a league before opening the draft lobby.', true);
    return;
  }
  if (current.draftLobbyOpen) {
    window.location.href = `draft.html?league=${encodeURIComponent(current.id)}`;
    return;
  }
  const updated = normalizeLeague({
    ...current,
    draftLobbyOpen: true,
    draftLobbyStartedAt: current.draftLobbyStartedAt || new Date().toISOString()
  });
  try {
    await saveLeagueToApi(updated);
    await refreshLeagueFromApi();
  } catch (error) {
    setSettingsStatus(mutationErrorMessage(error, 'Could not open draft lobby. No local changes were made.'), true);
    renderLeague();
    return;
  }
  renderLeague();
  window.location.href = `draft.html?league=${encodeURIComponent(updated.id)}`;
});
'''
league = replace_once(league, step_old, step_new, "separate lobby open and entry flow")
LEAGUE.write_text(league, encoding="utf-8")

league_html = LEAGUE_HTML.read_text(encoding="utf-8")
league_html = replace_once(
    league_html,
    '<a class="league-tab" href="draft.html">Draft Room</a>',
    '<a class="league-tab" id="league-draft-tab-link" href="draft.html">Draft Room</a>',
    "league draft tab link id",
)
league_html = replace_once(
    league_html,
    '<a class="button" href="draft.html">Open draft room</a>',
    '<a class="button" id="team-draft-link" href="draft.html">Open draft room</a>',
    "team draft link id",
)
overview_anchor = '''    <section class="card card--accent" data-league-panel="team">
'''
overview_card = '''    <section class="card" data-league-panel="overview">
      <div class="card__header">
        <div>
          <h2>Draft lobby</h2>
          <div class="muted small" id="draft-lobby-overview-status">Waiting for league status.</div>
        </div>
        <span class="pill pill--muted" id="draft-lobby-overview-badge">Closed</span>
      </div>
      <div class="cta-row">
        <a class="button button--primary" id="draft-lobby-overview-link" href="draft.html" hidden>Enter draft lobby</a>
        <a class="button button--ghost" href="league.html#managers">View managers</a>
      </div>
    </section>

''' + overview_anchor
if "id=\"draft-lobby-overview-status\"" not in league_html:
    league_html = replace_once(league_html, overview_anchor, overview_card, "league overview lobby card anchor")
LEAGUE_HTML.write_text(league_html, encoding="utf-8")

for path in (HANDLER, HEADER, ROUTES, STATE, DRAFT, DRAFT_HTML, LEAGUE, LEAGUE_HTML):
    text = path.read_text(encoding="utf-8")
    if "\r\n" in text:
        path.write_text(text.replace("\r\n", "\n"), encoding="utf-8")

print("multiplayer draft lobby and explicit start transformation applied")
