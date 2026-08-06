std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string canonicalEmail(std::string value) {
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), value.end());
    return lower(std::move(value));
}

std::string nextId(const std::string &prefix) {
    static std::atomic<unsigned long long> counter{0};
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return prefix + "-" + std::to_string(now) + "-" + std::to_string(++counter);
}

drogon::HttpResponsePtr jsonResponse(const Json::Value &payload,
                                     drogon::HttpStatusCode status = drogon::k200OK) {
    auto response = drogon::HttpResponse::newHttpJsonResponse(payload);
    response->setStatusCode(status);
    return response;
}

void sendError(std::function<void(const drogon::HttpResponsePtr &)> &callback,
               drogon::HttpStatusCode status,
               const std::string &code,
               const std::string &message,
               const std::vector<std::string> &blockers = {}) {
    Json::Value payload;
    payload["error"] = message;
    payload["code"] = code;
    if (!blockers.empty()) {
        payload["blockers"] = Json::Value{Json::arrayValue};
        for (const auto &blocker : blockers) payload["blockers"].append(blocker);
    }
    callback(jsonResponse(payload, status));
}

#ifdef CFF_HAS_POSTGRES
struct PgConnDeleter {
    void operator()(PGconn *conn) const {
        if (conn) PQfinish(conn);
    }
};

struct PgResultDeleter {
    void operator()(PGresult *result) const {
        if (result) PQclear(result);
    }
};

using PgConnPtr = std::unique_ptr<PGconn, PgConnDeleter>;
using PgResultPtr = std::unique_ptr<PGresult, PgResultDeleter>;

PgConnPtr connectToDatabase() {
    const auto *url = std::getenv("DB_URL");
    if (!url || std::string{url}.empty()) return nullptr;
    auto connection = PgConnPtr{PQconnectdb(url)};
    if (PQstatus(connection.get()) != CONNECTION_OK) {
        std::cerr << "[commissioner] Postgres connection failed: "
                  << PQerrorMessage(connection.get()) << std::endl;
        return nullptr;
    }
    return connection;
}

PgResultPtr execute(PGconn *connection,
                    const std::string &sql,
                    const std::vector<std::string> &parameters = {}) {
    std::vector<const char *> values;
    values.reserve(parameters.size());
    for (const auto &parameter : parameters) values.push_back(parameter.c_str());
    return PgResultPtr{PQexecParams(
        connection,
        sql.c_str(),
        static_cast<int>(values.size()),
        nullptr,
        values.empty() ? nullptr : values.data(),
        nullptr,
        nullptr,
        0)};
}

bool ok(PGresult *result, ExecStatusType expected) {
    return result && PQresultStatus(result) == expected;
}

std::string cell(PGresult *result, int row, int column) {
    if (!result || PQgetisnull(result, row, column)) return "";
    return PQgetvalue(result, row, column);
}

int cellInt(PGresult *result, int row, int column, int fallback = 0) {
    const auto value = cell(result, row, column);
    if (value.empty()) return fallback;
    try {
        return std::stoi(value);
    } catch (...) {
        return fallback;
    }
}

bool cellBool(PGresult *result, int row, int column) {
    return cell(result, row, column) == "t";
}

std::string jsonText(const Json::Value &value) {
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    return Json::writeString(writer, value);
}

Json::Value parseJson(const std::string &raw) {
    Json::Value value;
    Json::CharReaderBuilder reader;
    std::string errors;
    std::istringstream stream(raw);
    if (!Json::parseFromStream(reader, stream, &value, &errors)) {
        return Json::Value{Json::objectValue};
    }
    return value;
}

struct LeagueContext {
    bool exists{false};
    bool commissioner{false};
    bool owner{false};
    std::string ownerEmail;
    int teamCount{0};
    int activeManagers{0};
    int reservedManagers{0};
    bool draftStarted{false};
};

LeagueContext loadLeagueContext(PGconn *connection,
                                const std::string &leagueId,
                                const std::string &actorEmail,
                                bool lockLeague) {
    const auto sql =
        "SELECT l.account_email, l.team_count, "
        "EXISTS (SELECT 1 FROM league_members m WHERE m.league_id = l.id "
        "AND lower(m.email) = lower($2) AND m.role = 'commissioner' AND m.status = 'active'), "
        "(lower(l.account_email) = lower($2)), "
        "(SELECT COUNT(*) FROM league_members m WHERE m.league_id = l.id AND m.status = 'active'), "
        "(SELECT COUNT(*) FROM league_members m WHERE m.league_id = l.id AND m.status <> 'removed'), "
        "(EXISTS (SELECT 1 FROM draft_states d WHERE d.league_id = l.id AND d.status <> 'not_started') "
        " OR EXISTS (SELECT 1 FROM draft_picks p WHERE p.league_id = l.id)) "
        "FROM leagues l WHERE l.id = $1" + std::string(lockLeague ? " FOR UPDATE" : "");

    auto result = execute(connection, sql, {leagueId, actorEmail});
    LeagueContext context;
    if (!ok(result.get(), PGRES_TUPLES_OK) || PQntuples(result.get()) == 0) return context;
    context.exists = true;
    context.ownerEmail = canonicalEmail(cell(result.get(), 0, 0));
    context.teamCount = cellInt(result.get(), 0, 1);
    context.commissioner = cellBool(result.get(), 0, 2);
    context.owner = cellBool(result.get(), 0, 3);
    context.activeManagers = cellInt(result.get(), 0, 4);
    context.reservedManagers = cellInt(result.get(), 0, 5);
    context.draftStarted = cellBool(result.get(), 0, 6);
    return context;
}

cff::commissioner::MemberState loadMemberState(PGconn *connection,
                                                const std::string &leagueId,
                                                const std::string &memberEmail) {
    auto result = execute(
        connection,
        "SELECT m.status, (lower(l.account_email) = lower(m.email)), "
        "(SELECT COUNT(*) FROM rosters r WHERE r.league_id = m.league_id AND lower(r.manager_email) = lower(m.email)), "
        "(SELECT COUNT(*) FROM draft_picks p WHERE p.league_id = m.league_id AND lower(p.manager_email) = lower(m.email)), "
        "(SELECT COUNT(*) FROM league_matchups x WHERE x.league_id = m.league_id "
        " AND (lower(x.home_manager_email) = lower(m.email) OR lower(x.away_manager_email) = lower(m.email))), "
        "(SELECT COUNT(*) FROM trade_offers t WHERE t.league_id = m.league_id "
        " AND t.status IN ('pending', 'accepted') "
        " AND (lower(t.offered_by_email) = lower(m.email) OR lower(t.offered_to_email) = lower(m.email))), "
        "(SELECT COUNT(*) FROM waiver_claims w WHERE w.league_id = m.league_id "
        " AND w.status = 'pending' AND lower(w.manager_email) = lower(m.email)) "
        "FROM league_members m JOIN leagues l ON l.id = m.league_id "
        "WHERE m.league_id = $1 AND lower(m.email) = lower($2)",
        {leagueId, memberEmail});

    cff::commissioner::MemberState member;
    if (!ok(result.get(), PGRES_TUPLES_OK) || PQntuples(result.get()) == 0) return member;
    member.exists = true;
    member.status = cell(result.get(), 0, 0);
    member.owner = cellBool(result.get(), 0, 1);
    member.rosterPlayers = cellInt(result.get(), 0, 2);
    member.draftPicks = cellInt(result.get(), 0, 3);
    member.scheduledMatchups = cellInt(result.get(), 0, 4);
    member.openTrades = cellInt(result.get(), 0, 5);
    member.pendingWaivers = cellInt(result.get(), 0, 6);
    return member;
}

Json::Value membersPayload(PGconn *connection,
                           const std::string &leagueId,
                           const std::string &ownerEmail) {
    auto result = execute(
        connection,
        "SELECT m.email, m.team_name, m.role, m.status, "
        "COALESCE(to_char(m.created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), ''), "
        "(SELECT COUNT(*) FROM rosters r WHERE r.league_id = m.league_id AND lower(r.manager_email) = lower(m.email)), "
        "(SELECT COUNT(*) FROM waiver_claims w WHERE w.league_id = m.league_id "
        " AND w.status = 'pending' AND lower(w.manager_email) = lower(m.email)), "
        "(SELECT COUNT(*) FROM trade_offers t WHERE t.league_id = m.league_id "
        " AND t.status IN ('pending', 'accepted') "
        " AND (lower(t.offered_by_email) = lower(m.email) OR lower(t.offered_to_email) = lower(m.email))), "
        "(SELECT COUNT(*) FROM draft_picks p WHERE p.league_id = m.league_id AND lower(p.manager_email) = lower(m.email)), "
        "(SELECT COUNT(*) FROM league_matchups x WHERE x.league_id = m.league_id "
        " AND (lower(x.home_manager_email) = lower(m.email) OR lower(x.away_manager_email) = lower(m.email))) "
        "FROM league_members m WHERE m.league_id = $1 AND m.status <> 'removed' "
        "ORDER BY CASE WHEN lower(m.email) = lower($2) THEN 0 ELSE 1 END, m.created_at",
        {leagueId, ownerEmail});

    Json::Value members(Json::arrayValue);
    if (!ok(result.get(), PGRES_TUPLES_OK)) return members;
    for (int row = 0; row < PQntuples(result.get()); ++row) {
        Json::Value member;
        member["email"] = cell(result.get(), row, 0);
        member["teamName"] = cell(result.get(), row, 1);
        member["role"] = cell(result.get(), row, 2);
        const auto status = cell(result.get(), row, 3);
        member["status"] = status == "active" ? "Active"
                            : status == "pending" ? "Pending"
                            : status == "invited" ? "Invited"
                            : "Removed";
        member["createdAt"] = cell(result.get(), row, 4);
        member["rosterPlayers"] = cellInt(result.get(), row, 5);
        member["pendingWaivers"] = cellInt(result.get(), row, 6);
        member["openTrades"] = cellInt(result.get(), row, 7);
        member["draftPicks"] = cellInt(result.get(), row, 8);
        member["scheduledMatchups"] = cellInt(result.get(), row, 9);
        member["owner"] = canonicalEmail(member["email"].asString()) == canonicalEmail(ownerEmail);
        members.append(member);
    }
    return members;
}

Json::Value dashboardPayload(PGconn *connection,
                             const std::string &leagueId,
                             const std::string &actorEmail) {
    const auto context = loadLeagueContext(connection, leagueId, actorEmail, false);
    Json::Value payload;
    payload["leagueId"] = leagueId;
    payload["ownerEmail"] = context.ownerEmail;
    payload["actorIsOwner"] = context.owner;
    payload["actorIsCommissioner"] = context.commissioner;
    payload["teamCount"] = context.teamCount;
    payload["activeManagers"] = context.activeManagers;
    payload["reservedManagers"] = context.reservedManagers;
    payload["openTeamSlots"] = std::max(0, context.teamCount - context.reservedManagers);
    payload["draftStarted"] = context.draftStarted;
    payload["protectedSettingsLocked"] = context.draftStarted;
    payload["settingsLockReason"] = context.draftStarted
        ? "Core competition settings are locked because the draft has started."
        : "";
    payload["members"] = membersPayload(connection, leagueId, context.ownerEmail);
    return payload;
}

bool begin(PGconn *connection) {
    return ok(execute(connection, "BEGIN").get(), PGRES_COMMAND_OK);
}

bool commit(PGconn *connection) {
    return ok(execute(connection, "COMMIT").get(), PGRES_COMMAND_OK);
}

void rollback(PGconn *connection) {
    execute(connection, "ROLLBACK");
}

std::string operationKey(const drogon::HttpRequestPtr &request) {
    auto key = request->getHeader("Idempotency-Key");
    if (!key.empty()) return key;
    const auto body = request->getJsonObject();
    if (body && body->isMember("operationKey") && (*body)["operationKey"].isString()) {
        return (*body)["operationKey"].asString();
    }
    return nextId("commissioner-operation");
}

std::optional<Json::Value> replayedOperation(PGconn *connection,
                                             const std::string &leagueId,
                                             const std::string &key) {
    auto result = execute(
        connection,
        "SELECT response_payload::text FROM commissioner_operations "
        "WHERE league_id = $1 AND operation_key = $2",
        {leagueId, key});
    if (!ok(result.get(), PGRES_TUPLES_OK) || PQntuples(result.get()) == 0) return std::nullopt;
    return parseJson(cell(result.get(), 0, 0));
}

bool recordOperation(PGconn *connection,
                     const std::string &leagueId,
                     const std::string &key,
                     const std::string &type,
                     const Json::Value &payload) {
    auto result = execute(
        connection,
        "INSERT INTO commissioner_operations "
        "(league_id, operation_key, operation_type, response_payload) "
        "VALUES ($1, $2, $3, $4::jsonb) ON CONFLICT (league_id, operation_key) DO NOTHING",
        {leagueId, key, type, jsonText(payload)});
    return ok(result.get(), PGRES_COMMAND_OK);
}

void audit(PGconn *connection,
           const std::string &leagueId,
           const std::string &actorEmail,
           const std::string &summary,
           const Json::Value &metadata) {
    execute(
        connection,
        "INSERT INTO transactions "
        "(id, league_id, manager_email, transaction_type, summary, metadata) "
        "VALUES ($1, $2, $3, 'Commissioner', $4, $5::jsonb)",
        {nextId("txn"), leagueId, actorEmail, summary, jsonText(metadata)});
}

drogon::HttpStatusCode statusForDecision(const cff::commissioner::Decision &decision) {
    if (decision.code == "member_not_found") return drogon::k404NotFound;
    if (decision.code == "owner_required" || decision.code == "owner_protected") {
        return drogon::k403Forbidden;
    }
    return drogon::k409Conflict;
}

bool validEmail(const std::string &email) {
    const auto at = email.find('@');
    return at > 0 && at + 1 < email.size() && email.find(' ', 0) == std::string::npos;
}

#endif // CFF_HAS_POSTGRES

