struct PgConnDeleter {
    void operator()(PGconn *connection) const {
        if (connection) PQfinish(connection);
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
    if (!connection || PQstatus(connection.get()) != CONNECTION_OK) return nullptr;
    return connection;
}

PgResultPtr execute(
    PGconn *connection,
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

bool tuplesOk(PGresult *result) {
    return result && PQresultStatus(result) == PGRES_TUPLES_OK;
}

std::string cell(PGresult *result, int row, int column) {
    if (!result || row < 0 || column < 0 || PQgetisnull(result, row, column)) return "";
    return PQgetvalue(result, row, column);
}

int cellInt(PGresult *result, int row, int column, int fallback = 0) {
    try {
        const auto value = cell(result, row, column);
        return value.empty() ? fallback : std::stoi(value);
    } catch (...) {
        return fallback;
    }
}

double cellDouble(PGresult *result, int row, int column, double fallback = 0.0) {
    try {
        const auto value = cell(result, row, column);
        return value.empty() ? fallback : std::stod(value);
    } catch (...) {
        return fallback;
    }
}

bool cellBool(PGresult *result, int row, int column) {
    return cell(result, row, column) == "t";
}

void markUnavailable(Json::Value &freshness, const std::string &section) {
    freshness["unavailableSections"].append(section);
    freshness["partial"] = true;
}

struct Context {
    std::string name;
    int teamCount{0};
    std::string scoring;
    std::string draftType;
    std::string draftDate;
    bool draftLobbyOpen{false};
    Json::Value rosterRules{Json::objectValue};
    Json::Value waiverRules{Json::objectValue};
    std::string notes;
    std::string teamName;
    std::string role;
    bool owner{false};
    int activeManagers{0};
};

std::optional<Context> loadContext(
    PGconn *connection,
    const std::string &leagueId,
    const std::string &actorEmail) {
    auto result = execute(
        connection,
        "SELECT l.name, l.team_count, l.scoring, l.draft_type, "
        "COALESCE(to_char(l.draft_date AT TIME ZONE 'UTC', 'YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"'), ''), "
        "l.draft_lobby_open, l.roster_rules::text, l.waiver_rules::text, l.notes, "
        "m.team_name, m.role, (lower(l.account_email) = lower($2)), "
        "(SELECT COUNT(*) FROM league_members active "
        " WHERE active.league_id = l.id AND active.status = 'active') "
        "FROM leagues l JOIN league_members m ON m.league_id = l.id "
        "WHERE l.id = $1 AND lower(m.email) = lower($2) AND m.status = 'active'",
        {leagueId, actorEmail});
    if (!tuplesOk(result.get()) || PQntuples(result.get()) == 0) return std::nullopt;

    Context context;
    context.name = cell(result.get(), 0, 0);
    context.teamCount = cellInt(result.get(), 0, 1);
    context.scoring = cell(result.get(), 0, 2);
    context.draftType = cell(result.get(), 0, 3);
    context.draftDate = cell(result.get(), 0, 4);
    context.draftLobbyOpen = cellBool(result.get(), 0, 5);
    context.rosterRules = parseJson(cell(result.get(), 0, 6));
    context.waiverRules = parseJson(cell(result.get(), 0, 7));
    context.notes = cell(result.get(), 0, 8);
    context.teamName = cell(result.get(), 0, 9);
    context.role = cell(result.get(), 0, 10);
    context.owner = cellBool(result.get(), 0, 11);
    context.activeManagers = cellInt(result.get(), 0, 12);
    return context;
}

Json::Value leaguePayload(const Context &context, const std::string &leagueId) {
    Json::Value league(Json::objectValue);
    league["id"] = leagueId;
    league["name"] = context.name;
    league["teamCount"] = context.teamCount;
    league["activeManagers"] = context.activeManagers;
    league["scoring"] = context.scoring;
    league["draftType"] = context.draftType;
    league["draftDate"] = context.draftDate;
    league["draftLobbyOpen"] = context.draftLobbyOpen;
    league["teamName"] = context.teamName;
    league["role"] = context.owner ? "owner" : context.role;
    league["isCommissioner"] = context.owner || context.role == "commissioner";
    return league;
}

