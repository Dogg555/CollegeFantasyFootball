if (resultOk(counts.get(), PGRES_TUPLES_OK) && PQntuples(counts.get()) > 0) {
    using JsonInt = Json::Int64;
    payload["counts"]["teams"] = static_cast<JsonInt>(std::stoll(PQgetvalue(counts.get(), 0, 0)));
    payload["counts"]["players"] = static_cast<JsonInt>(std::stoll(PQgetvalue(counts.get(), 0, 1)));
    payload["counts"]["games"] = static_cast<JsonInt>(std::stoll(PQgetvalue(counts.get(), 0, 2)));
    payload["counts"]["playerStats"] = static_cast<JsonInt>(std::stoll(PQgetvalue(counts.get(), 0, 3)));
}

if (resultOk(runs.get(), PGRES_TUPLES_OK)) {
    for (int row = 0; row < PQntuples(runs.get()); ++row) {
        Json::Value run;
        // ingestion_runs.id may be a numeric id; store explicitly as Json::Int64 to avoid ambiguous conversion
        using JsonInt = Json::Int64;
        run["id"] = static_cast<JsonInt>(std::stoll(PQgetvalue(runs.get(), row, 0)));
        run["resource"] = PQgetvalue(runs.get(), row, 1);
        run["season"] = std::stoi(PQgetvalue(runs.get(), row, 2));
        run["week"] = std::stoi(PQgetvalue(runs.get(), row, 3));
        run["startedAt"] = PQgetvalue(runs.get(), row, 4);
