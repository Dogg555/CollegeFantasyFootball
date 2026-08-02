from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(path):
    return (ROOT / path).read_text(encoding="utf-8")


def write(path, content):
    target = ROOT / path
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(content, encoding="utf-8")


def replace_once(path, old, new):
    content = read(path)
    if old not in content:
        raise AssertionError(f"Expected anchor not found in {path}: {old[:120]!r}")
    write(path, content.replace(old, new, 1))


def replace_all(path, old, new):
    content = read(path)
    if old not in content:
        raise AssertionError(f"Expected value not found in {path}: {old!r}")
    write(path, content.replace(old, new))


# Weekly roster refresh: Tuesday 10:00 UTC.
replace_once("render.yaml", '    schedule: "0 8 * * 1"', '    schedule: "0 10 * * 2"')
replace_once(".github/workflows/ci.yml", "          assert roster['schedule'] == '0 8 * * 1'", "          assert roster['schedule'] == '0 10 * * 2'")
replace_all("docs/alpha-release-runbook.md", "0 8 * * 1", "0 10 * * 2")
replace_all("docs/alpha-release-runbook.md", "Monday", "Tuesday")

# Player catalog API: team filter, pagination, and public catalog metadata.
replace_once(
    "backend/src/player_catalog.h",
    "std::vector<PlayerCard> searchPlayers(const std::string &query,\n"
    "                                      const std::optional<std::string> &positionFilter,\n"
    "                                      const std::optional<std::string> &conferenceFilter,\n"
    "                                      std::size_t limit = 25);\n",
    "std::vector<PlayerCard> searchPlayers(const std::string &query,\n"
    "                                      const std::optional<std::string> &positionFilter,\n"
    "                                      const std::optional<std::string> &conferenceFilter,\n"
    "                                      const std::optional<std::string> &teamFilter,\n"
    "                                      std::size_t limit = 25,\n"
    "                                      std::size_t offset = 0);\n\n"
    "// Public, non-sensitive summary used by the player browser to show roster\n"
    "// coverage and sync freshness without exposing admin ingestion details.\n"
    "Json::Value playerCatalogMeta();\n"
)

replace_once(
    "backend/src/player_catalog.cpp",
    "std::size_t clampLimit(std::size_t limit) {\n"
    "    constexpr std::size_t kMax = 100;\n"
    "    constexpr std::size_t kDefault = 25;\n"
    "    if (limit == 0) return kDefault;\n"
    "    return std::min(limit, kMax);\n"
    "}\n",
    "std::size_t clampLimit(std::size_t limit) {\n"
    "    constexpr std::size_t kMax = 100;\n"
    "    constexpr std::size_t kDefault = 25;\n"
    "    if (limit == 0) return kDefault;\n"
    "    return std::min(limit, kMax);\n"
    "}\n\n"
    "std::size_t clampOffset(std::size_t offset) {\n"
    "    constexpr std::size_t kMax = 5000;\n"
    "    return std::min(offset, kMax);\n"
    "}\n"
)

replace_once(
    "backend/src/player_catalog.cpp",
    "std::vector<PlayerCard> searchPlayers(const std::string &query,\n"
    "                                      const std::optional<std::string> &positionFilter,\n"
    "                                      const std::optional<std::string> &conferenceFilter,\n"
    "                                      std::size_t limit) {",
    "std::vector<PlayerCard> searchPlayers(const std::string &query,\n"
    "                                      const std::optional<std::string> &positionFilter,\n"
    "                                      const std::optional<std::string> &conferenceFilter,\n"
    "                                      const std::optional<std::string> &teamFilter,\n"
    "                                      std::size_t limit,\n"
    "                                      std::size_t offset) {"
)
replace_once("backend/src/player_catalog.cpp", "    params.reserve(tokens.size() + 3);", "    params.reserve(tokens.size() + 5);")
replace_once(
    "backend/src/player_catalog.cpp",
    "    if (conferenceFilter && !conferenceFilter->empty()) {\n"
    "        params.push_back(*conferenceFilter);\n"
    "        whereClauses.push_back(\"player.conference ILIKE $\" + std::to_string(params.size()));\n"
    "    }\n",
    "    if (conferenceFilter && !conferenceFilter->empty()) {\n"
    "        params.push_back(*conferenceFilter);\n"
    "        whereClauses.push_back(\"player.conference ILIKE $\" + std::to_string(params.size()));\n"
    "    }\n\n"
    "    if (teamFilter && !teamFilter->empty()) {\n"
    "        params.push_back(*teamFilter);\n"
    "        whereClauses.push_back(\"player.team ILIKE $\" + std::to_string(params.size()));\n"
    "    }\n"
)
replace_once(
    "backend/src/player_catalog.cpp",
    "    sql += R\"SQL(\n"
    "        ORDER BY\n"
    "            player.season DESC NULLS LAST,\n"
    "            CASE UPPER(COALESCE(player.position, ''))\n"
    "                WHEN 'QB' THEN 1\n"
    "                WHEN 'RB' THEN 2\n"
    "                WHEN 'WR' THEN 3\n"
    "                WHEN 'TE' THEN 4\n"
    "                WHEN 'K' THEN 5\n"
    "                ELSE 6\n"
    "            END,\n"
    "            player.full_name ASC\n"
    "        LIMIT $\n"
    "    )SQL\";\n"
    "    sql += std::to_string(params.size() + 1);\n\n"
    "    params.push_back(std::to_string(clampLimit(limit)));",
    "    sql += R\"SQL(\n"
    "        ORDER BY\n"
    "            player.season DESC NULLS LAST,\n"
    "            CASE UPPER(COALESCE(player.position, ''))\n"
    "                WHEN 'QB' THEN 1\n"
    "                WHEN 'RB' THEN 2\n"
    "                WHEN 'WR' THEN 3\n"
    "                WHEN 'TE' THEN 4\n"
    "                WHEN 'K' THEN 5\n"
    "                ELSE 6\n"
    "            END,\n"
    "            player.full_name ASC\n"
    "        LIMIT $\n"
    "    )SQL\";\n"
    "    sql += std::to_string(params.size() + 1);\n"
    "    sql += \" OFFSET $\" + std::to_string(params.size() + 2);\n\n"
    "    params.push_back(std::to_string(clampLimit(limit)));\n"
    "    params.push_back(std::to_string(clampOffset(offset)));"
)
replace_once(
    "backend/src/player_catalog.cpp",
    "    (void)conferenceFilter;\n    (void)limit;",
    "    (void)conferenceFilter;\n    (void)teamFilter;\n    (void)limit;\n    (void)offset;"
)
replace_once(
    "backend/src/player_catalog.cpp",
    "    return results;\n}\n\n} // namespace cff\n",
    "    return results;\n}\n\n"
    "Json::Value playerCatalogMeta() {\n"
    "    Json::Value payload;\n"
    "#ifdef CFF_HAS_POSTGRES\n"
    "    auto connection = connectToDb();\n"
    "    payload[\"databaseConfigured\"] = static_cast<bool>(connection);\n"
    "    if (!connection) {\n"
    "        payload[\"status\"] = \"unavailable\";\n"
    "        return payload;\n"
    "    }\n\n"
    "    PgResultPtr summary{PQexec(connection.get(), R\"SQL(\n"
    "        SELECT COUNT(*), COALESCE(MAX(season), 0),\n"
    "               COALESCE(MAX(updated_at)::text, ''),\n"
    "               COUNT(DISTINCT NULLIF(team, '')),\n"
    "               COUNT(DISTINCT NULLIF(conference, ''))\n"
    "        FROM players WHERE active = TRUE\n"
    "    )SQL\")};\n"
    "    if (PQresultStatus(summary.get()) != PGRES_TUPLES_OK || PQntuples(summary.get()) == 0) {\n"
    "        payload[\"status\"] = \"unavailable\";\n"
    "        return payload;\n"
    "    }\n"
    "    payload[\"status\"] = \"ok\";\n"
    "    payload[\"activePlayers\"] = static_cast<Json::Int64>(std::atoll(PQgetvalue(summary.get(), 0, 0)));\n"
    "    payload[\"season\"] = std::atoi(PQgetvalue(summary.get(), 0, 1));\n"
    "    payload[\"lastUpdated\"] = PQgetvalue(summary.get(), 0, 2);\n"
    "    payload[\"teams\"] = static_cast<Json::Int64>(std::atoll(PQgetvalue(summary.get(), 0, 3)));\n"
    "    payload[\"conferences\"] = static_cast<Json::Int64>(std::atoll(PQgetvalue(summary.get(), 0, 4)));\n\n"
    "    Json::Value positions(Json::objectValue);\n"
    "    PgResultPtr positionRows{PQexec(connection.get(), R\"SQL(\n"
    "        SELECT UPPER(COALESCE(NULLIF(position, ''), 'OTHER')), COUNT(*)\n"
    "        FROM players WHERE active = TRUE\n"
    "        GROUP BY 1 ORDER BY 2 DESC, 1 ASC\n"
    "    )SQL\")};\n"
    "    if (PQresultStatus(positionRows.get()) == PGRES_TUPLES_OK) {\n"
    "        for (int row = 0; row < PQntuples(positionRows.get()); ++row) {\n"
    "            positions[PQgetvalue(positionRows.get(), row, 0)] =\n"
    "                static_cast<Json::Int64>(std::atoll(PQgetvalue(positionRows.get(), row, 1)));\n"
    "        }\n"
    "    }\n"
    "    payload[\"positions\"] = positions;\n"
    "#else\n"
    "    payload[\"databaseConfigured\"] = false;\n"
    "    payload[\"status\"] = \"unavailable\";\n"
    "#endif\n"
    "    return payload;\n"
    "}\n\n} // namespace cff\n"
)

# Public schedule freshness metadata.
replace_once(
    "backend/src/live_scores.h",
    "Json::Value cachedLiveScorePayload();\nJson::Value liveScoreIngestStatus();",
    "Json::Value cachedLiveScorePayload();\nJson::Value cachedLiveScoreMeta();\nJson::Value liveScoreIngestStatus();"
)
replace_once(
    "backend/src/live_scores.cpp",
    "Json::Value liveScoreIngestStatus() {",
    "Json::Value cachedLiveScoreMeta() {\n"
    "    Json::Value payload;\n"
    "    const auto dbUrl = env(\"DB_URL\");\n"
    "    payload[\"databaseConfigured\"] = dbUrl.has_value();\n"
    "    if (!dbUrl) {\n"
    "        payload[\"status\"] = \"unavailable\";\n"
    "        return payload;\n"
    "    }\n"
    "    try {\n"
    "        pqxx::connection connection{*dbUrl};\n"
    "        pqxx::read_transaction transaction{connection};\n"
    "        const auto rows = transaction.exec(\n"
    "            \"SELECT status,COALESCE(fetched_at::text,''),game_count,live_game_count,\"\n"
    "            \"COALESCE(EXTRACT(EPOCH FROM(NOW()-fetched_at))::bigint,-1),\"\n"
    "            \"COALESCE(schedule_fetched_at::text,''),schedule_game_count,\"\n"
    "            \"COALESCE(EXTRACT(EPOCH FROM(NOW()-schedule_fetched_at))::bigint,-1) \"\n"
    "            \"FROM live_score_cache WHERE id=1\"\n"
    "        );\n"
    "        if (rows.empty()) {\n"
    "            payload[\"status\"] = \"never\";\n"
    "            payload[\"gameCount\"] = 0;\n"
    "            payload[\"liveGameCount\"] = 0;\n"
    "            payload[\"scheduleGameCount\"] = 0;\n"
    "            payload[\"fresh\"] = false;\n"
    "            payload[\"scheduleFresh\"] = false;\n"
    "            return payload;\n"
    "        }\n"
    "        const auto age = rows[0][4].as<long long>();\n"
    "        const auto scheduleAge = rows[0][7].as<long long>();\n"
    "        payload[\"status\"] = rows[0][0].c_str();\n"
    "        payload[\"fetchedAt\"] = rows[0][1].c_str();\n"
    "        payload[\"gameCount\"] = rows[0][2].as<int>();\n"
    "        payload[\"liveGameCount\"] = rows[0][3].as<int>();\n"
    "        payload[\"ageSeconds\"] = static_cast<Json::Int64>(age);\n"
    "        payload[\"scheduleFetchedAt\"] = rows[0][5].c_str();\n"
    "        payload[\"scheduleGameCount\"] = rows[0][6].as<int>();\n"
    "        payload[\"scheduleAgeSeconds\"] = static_cast<Json::Int64>(scheduleAge);\n"
    "        payload[\"fresh\"] = age >= 0 && age <= 600;\n"
    "        payload[\"scheduleFresh\"] = scheduleAge >= 0 &&\n"
    "            scheduleAge <= static_cast<long long>(refreshHours()) * 7200;\n"
    "        return payload;\n"
    "    } catch (const std::exception &error) {\n"
    "        payload[\"status\"] = \"unavailable\";\n"
    "        return payload;\n"
    "    }\n"
    "}\n\nJson::Value liveScoreIngestStatus() {"
)

# Register new routes and make player browsing truly optional-query/paginated.
replace_once(
    "backend/src/main.cpp",
    "        .registerHandler(\"/api/scores/live\",\n"
    "                         [](const drogon::HttpRequestPtr&, std::function<void (const drogon::HttpResponsePtr &)> &&callback) {\n"
    "                             auto resp = drogon::HttpResponse::newHttpJsonResponse(cff::cachedLiveScorePayload());\n"
    "                             resp->setStatusCode(drogon::k200OK);\n"
    "                             callback(resp);\n"
    "                         },\n"
    "                         {drogon::Get})\n",
    "        .registerHandler(\"/api/scores/live\",\n"
    "                         [](const drogon::HttpRequestPtr&, std::function<void (const drogon::HttpResponsePtr &)> &&callback) {\n"
    "                             auto resp = drogon::HttpResponse::newHttpJsonResponse(cff::cachedLiveScorePayload());\n"
    "                             resp->setStatusCode(drogon::k200OK);\n"
    "                             callback(resp);\n"
    "                         },\n"
    "                         {drogon::Get})\n"
    "        .registerHandler(\"/api/scores/live/meta\",\n"
    "                         [](const drogon::HttpRequestPtr&, std::function<void (const drogon::HttpResponsePtr &)> &&callback) {\n"
    "                             auto resp = drogon::HttpResponse::newHttpJsonResponse(cff::cachedLiveScoreMeta());\n"
    "                             resp->setStatusCode(drogon::k200OK);\n"
    "                             callback(resp);\n"
    "                         },\n"
    "                         {drogon::Get})\n"
    "        .registerHandler(\"/api/players/meta\",\n"
    "                         [](const drogon::HttpRequestPtr&, std::function<void (const drogon::HttpResponsePtr &)> &&callback) {\n"
    "                             auto resp = drogon::HttpResponse::newHttpJsonResponse(cff::playerCatalogMeta());\n"
    "                             resp->setStatusCode(drogon::k200OK);\n"
    "                             callback(resp);\n"
    "                         },\n"
    "                         {drogon::Get})\n"
)
replace_once(
    "backend/src/main.cpp",
    "                             const auto query = req->getParameter(\"query\");\n"
    "                             if (query.empty()) {\n"
    "                                 Json::Value error;\n"
    "                                 error[\"error\"] = \"Query parameter is required\";\n"
    "                                 auto resp = drogon::HttpResponse::newHttpJsonResponse(error);\n"
    "                                 resp->setStatusCode(drogon::k400BadRequest);\n"
    "                                 callback(resp);\n"
    "                                 return;\n"
    "                             }\n\n"
    "                             auto positionFilter = getOptionalParam(req, \"position\");\n"
    "                             auto conferenceFilter = getOptionalParam(req, \"conference\");\n",
    "                             const auto query = req->getParameter(\"query\");\n"
    "                             auto positionFilter = getOptionalParam(req, \"position\");\n"
    "                             auto conferenceFilter = getOptionalParam(req, \"conference\");\n"
    "                             auto teamFilter = getOptionalParam(req, \"team\");\n"
)
replace_once("backend/src/main.cpp", "limit = std::min<std::size_t>(parsed, 50);", "limit = std::min<std::size_t>(parsed, 100);")
replace_once(
    "backend/src/main.cpp",
    "                             const auto results = cff::searchPlayers(query, positionFilter, conferenceFilter, limit);",
    "                             std::size_t offset = 0;\n"
    "                             const auto offsetParam = req->getParameter(\"offset\");\n"
    "                             if (!offsetParam.empty()) {\n"
    "                                 char *end = nullptr;\n"
    "                                 const auto parsed = std::strtoul(offsetParam.c_str(), &end, 10);\n"
    "                                 if (end != offsetParam.c_str()) offset = std::min<std::size_t>(parsed, 5000);\n"
    "                             }\n\n"
    "                             const auto results = cff::searchPlayers(\n"
    "                                 query, positionFilter, conferenceFilter, teamFilter, limit, offset\n"
    "                             );"
)
replace_once(
    "backend/src/main.cpp",
    "        .registerHandler(\"/api/scores/live\", preflightHandler, {drogon::Options})\n",
    "        .registerHandler(\"/api/scores/live\", preflightHandler, {drogon::Options})\n"
    "        .registerHandler(\"/api/scores/live/meta\", preflightHandler, {drogon::Options})\n"
)
replace_once(
    "backend/src/main.cpp",
    "        .registerHandler(\"/api/players\", preflightHandler, {drogon::Options})\n",
    "        .registerHandler(\"/api/players\", preflightHandler, {drogon::Options})\n"
    "        .registerHandler(\"/api/players/meta\", preflightHandler, {drogon::Options})\n"
)

# Shared Alpha responsive UI layer.
write("frontend/alpha-ui.css", r'''/* Alpha release responsive dashboards */
:root {
  --alpha-gap: clamp(12px, 2vw, 22px);
  --alpha-sticky-offset: 76px;
}

.league-tabs {
  position: sticky;
  top: 0;
  z-index: 20;
  overflow-x: auto;
  overscroll-behavior-inline: contain;
  scrollbar-width: thin;
  background: color-mix(in srgb, var(--surface, #fff) 94%, transparent);
  backdrop-filter: blur(12px);
}

.league-tab-select-wrap {
  display: none;
  margin-bottom: 14px;
}

.league-tab-select {
  width: 100%;
  min-height: 44px;
}

.league-dashboard,
.draft-dashboard {
  align-items: start;
  gap: var(--alpha-gap);
}

.league-dashboard {
  grid-template-columns: repeat(2, minmax(0, 1fr));
}

.league-dashboard > .card--accent,
.league-dashboard > #commissioner-settings,
.league-dashboard > #commissioner-locked {
  grid-column: 1 / -1;
}

.draft-dashboard {
  display: grid;
  grid-template-columns: minmax(0, 1.35fr) minmax(300px, 0.65fr);
}

.draft-primary {
  grid-column: 1;
}

.draft-secondary,
.draft-side {
  grid-column: 2;
}

.draft-roster-card {
  grid-row: span 2;
}

.card__header .mobile-card-toggle {
  display: none;
  min-width: 44px;
  min-height: 40px;
}

.alpha-status-dock {
  display: none;
}

.catalog-meta {
  display: grid;
  grid-template-columns: repeat(4, minmax(0, 1fr));
  gap: 10px;
  margin: 14px 0;
}

.catalog-stat {
  padding: 12px;
  border: 1px solid var(--border, #d8dee9);
  border-radius: 12px;
  background: color-mix(in srgb, var(--surface, #fff) 92%, var(--accent, #2563eb) 8%);
}

.catalog-stat strong,
.catalog-stat span {
  display: block;
}

.catalog-stat strong {
  font-size: 1.15rem;
}

.player-results-footer,
.scoreboard-freshness {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
  flex-wrap: wrap;
  margin-top: 12px;
}

.data-freshness {
  display: inline-flex;
  align-items: center;
  gap: 7px;
  padding: 6px 10px;
  border-radius: 999px;
  background: color-mix(in srgb, #16a34a 12%, transparent);
}

.data-freshness::before {
  content: '';
  width: 8px;
  height: 8px;
  border-radius: 50%;
  background: #16a34a;
}

.data-freshness.is-stale {
  background: color-mix(in srgb, #d97706 14%, transparent);
}

.data-freshness.is-stale::before {
  background: #d97706;
}

@media (max-width: 900px) {
  .league-dashboard,
  .draft-dashboard {
    grid-template-columns: 1fr;
  }

  .league-dashboard > *,
  .draft-primary,
  .draft-secondary,
  .draft-side {
    grid-column: 1;
  }

  .catalog-meta {
    grid-template-columns: repeat(2, minmax(0, 1fr));
  }
}

@media (max-width: 720px) {
  :root {
    --alpha-sticky-offset: 58px;
  }

  .league-tabs {
    display: none;
  }

  .league-tab-select-wrap {
    display: block;
    position: sticky;
    top: 0;
    z-index: 21;
    padding: 8px 0;
    background: var(--surface, #fff);
  }

  .layout,
  .layout--wide {
    padding-inline: 12px;
  }

  .card {
    border-radius: 14px;
  }

  .row {
    align-items: flex-start;
    gap: 10px;
  }

  .row > .actions,
  .row > button,
  .row > a.button {
    width: 100%;
  }

  .row .actions {
    display: grid;
    grid-template-columns: repeat(2, minmax(0, 1fr));
  }

  .row .actions .button {
    width: 100%;
  }

  [data-mobile-collapsible] .mobile-card-toggle {
    display: inline-flex;
  }

  [data-mobile-collapsible].is-collapsed > :not(.card__header) {
    display: none !important;
  }

  .alpha-status-dock {
    position: sticky;
    bottom: 10px;
    z-index: 30;
    display: grid;
    grid-template-columns: 1fr auto;
    gap: 4px 12px;
    margin: 0 12px 12px;
    padding: 12px 14px;
    border: 1px solid var(--border, #d8dee9);
    border-radius: 14px;
    background: color-mix(in srgb, var(--surface, #fff) 95%, transparent);
    box-shadow: 0 14px 35px rgb(15 23 42 / 18%);
    backdrop-filter: blur(14px);
  }

  .alpha-status-dock__manager {
    font-weight: 700;
  }

  .alpha-status-dock__clock {
    grid-row: 1 / span 2;
    grid-column: 2;
    align-self: center;
    font-size: 1.15rem;
    font-weight: 800;
  }

  .catalog-meta {
    grid-template-columns: 1fr 1fr;
  }

  .search {
    grid-template-columns: 1fr;
  }

  .search .button,
  .search select,
  .search input {
    width: 100%;
    min-height: 44px;
  }
}

@media (max-width: 440px) {
  .catalog-meta,
  .row .actions {
    grid-template-columns: 1fr;
  }
}
''')

write("frontend/alpha-ui.js", r'''(function initAlphaUi(root) {
  'use strict';

  function formatAge(value) {
    const seconds = Number(value);
    if (!Number.isFinite(seconds) || seconds < 0) return 'unknown';
    if (seconds < 60) return `${Math.round(seconds)} sec ago`;
    if (seconds < 3600) return `${Math.round(seconds / 60)} min ago`;
    if (seconds < 86400) return `${Math.round(seconds / 3600)} hr ago`;
    return `${Math.round(seconds / 86400)} day${Math.round(seconds / 86400) === 1 ? '' : 's'} ago`;
  }

  const helpers = { formatAge };
  if (typeof module !== 'undefined' && module.exports) module.exports = helpers;
  if (typeof document === 'undefined') return;

  function setupLeagueMobileNav() {
    const tabs = document.querySelector('.league-tabs');
    if (!tabs || document.querySelector('.league-tab-select')) return;
    const targets = [...tabs.querySelectorAll('.league-tab')];
    if (!targets.length) return;

    const wrap = document.createElement('div');
    wrap.className = 'league-tab-select-wrap layout';
    wrap.style.maxWidth = '1080px';
    const label = document.createElement('label');
    label.className = 'field';
    label.innerHTML = '<span>League section</span>';
    const select = document.createElement('select');
    select.className = 'league-tab-select';
    select.setAttribute('aria-label', 'League section');
    targets.forEach((target, index) => {
      const option = document.createElement('option');
      option.value = String(index);
      option.textContent = target.textContent.trim();
      option.selected = target.classList.contains('is-active');
      select.appendChild(option);
    });
    label.appendChild(select);
    wrap.appendChild(label);
    tabs.closest('main')?.insertAdjacentElement('afterend', wrap);

    select.addEventListener('change', () => {
      const target = targets[Number(select.value)];
      if (!target) return;
      if (target.tagName === 'A') {
        root.location.href = target.href;
      } else {
        target.click();
      }
    });

    const sync = () => {
      const activeIndex = targets.findIndex((target) => target.classList.contains('is-active'));
      if (activeIndex >= 0) select.value = String(activeIndex);
    };
    targets.forEach((target) => new MutationObserver(sync).observe(target, { attributes: true, attributeFilter: ['class'] }));
  }

  function setupCollapsibleCards() {
    const cards = [...document.querySelectorAll('[data-mobile-collapsible]')];
    cards.forEach((card) => {
      const header = card.querySelector(':scope > .card__header');
      if (!header || header.querySelector('.mobile-card-toggle')) return;
      const button = document.createElement('button');
      button.type = 'button';
      button.className = 'button button--ghost mobile-card-toggle';
      button.textContent = 'Show';
      button.setAttribute('aria-expanded', 'false');
      header.appendChild(button);

      const applyDefault = () => {
        if (root.matchMedia('(max-width: 720px)').matches && !card.dataset.mobileExpanded) {
          card.classList.add('is-collapsed');
        } else if (!root.matchMedia('(max-width: 720px)').matches) {
          card.classList.remove('is-collapsed');
        }
        const expanded = !card.classList.contains('is-collapsed');
        button.textContent = expanded ? 'Hide' : 'Show';
        button.setAttribute('aria-expanded', String(expanded));
      };

      button.addEventListener('click', () => {
        card.dataset.mobileExpanded = 'true';
        card.classList.toggle('is-collapsed');
        applyDefault();
      });
      root.addEventListener('resize', applyDefault, { passive: true });
      applyDefault();
    });
  }

  function setupDraftStatusDock() {
    const content = document.getElementById('draft-room-content');
    if (!content || document.getElementById('draft-mobile-status')) return;
    const dock = document.createElement('div');
    dock.id = 'draft-mobile-status';
    dock.className = 'alpha-status-dock';
    dock.setAttribute('aria-live', 'polite');
    dock.innerHTML = '<span class="alpha-status-dock__manager">Manager TBD</span><span class="muted small alpha-status-dock__state">Waiting</span><span class="alpha-status-dock__clock">--</span>';
    content.appendChild(dock);

    const manager = document.getElementById('draft-current-manager');
    const status = document.getElementById('draft-status');
    const clock = document.getElementById('draft-clock');
    const update = () => {
      dock.querySelector('.alpha-status-dock__manager').textContent = manager?.textContent || 'Manager TBD';
      dock.querySelector('.alpha-status-dock__state').textContent = status?.textContent || 'Waiting';
      dock.querySelector('.alpha-status-dock__clock').textContent = clock?.textContent || '--';
    };
    [manager, status, clock].filter(Boolean).forEach((node) => new MutationObserver(update).observe(node, { childList: true, subtree: true }));
    update();
  }

  function init() {
    document.documentElement.classList.add('alpha-ui-ready');
    setupLeagueMobileNav();
    setupCollapsibleCards();
    setupDraftStatusDock();
  }

  root.CFF_ALPHA_UI = helpers;
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', init);
  else init();
})(typeof window !== 'undefined' ? window : globalThis);
''')

# HTML wiring and dashboard semantics.
for page in ("frontend/index.html", "frontend/players.html", "frontend/league.html", "frontend/draft.html"):
    replace_once(page, '  <link rel="stylesheet" href="style.css" />', '  <link rel="stylesheet" href="style.css" />\n  <link rel="stylesheet" href="alpha-ui.css" />')

replace_once("frontend/index.html", '<div class="scoreboard-meta muted small" id="scoreboard-meta" aria-live="polite">Loading schedule…</div>', '<div class="scoreboard-meta muted small" id="scoreboard-meta" aria-live="polite">Loading schedule…</div>\n      <div class="scoreboard-freshness muted small" id="scoreboard-freshness" aria-live="polite">Checking cache freshness…</div>')
replace_once("frontend/index.html", '  <script src="scoreboard.js"></script>', '  <script src="alpha-ui.js"></script>\n  <script src="scoreboard.js"></script>')

replace_once("frontend/draft.html", '  <main class="layout layout--wide">', '  <main class="layout layout--wide draft-dashboard">')
replace_once("frontend/draft.html", '    <section class="card">\n      <div class="card__header">\n        <h2>Draft Queue</h2>', '    <section class="card draft-primary draft-queue-card">\n      <div class="card__header">\n        <h2>Draft Queue</h2>')
replace_once("frontend/draft.html", '    <section class="card card--accent">\n      <div class="card__header">\n        <h2>Your Roster</h2>', '    <section class="card card--accent draft-primary draft-roster-card">\n      <div class="card__header">\n        <h2>Your Roster</h2>')
replace_once("frontend/draft.html", '    <section class="card">\n      <div class="card__header">\n        <h2>Draft Order</h2>', '    <section class="card draft-secondary" data-mobile-collapsible>\n      <div class="card__header">\n        <h2>Draft Order</h2>')
replace_once("frontend/draft.html", '    <section class="card">\n      <div class="card__header">\n        <h2>Draft Picks</h2>', '    <section class="card draft-secondary" data-mobile-collapsible>\n      <div class="card__header">\n        <h2>Draft Picks</h2>')
replace_once("frontend/draft.html", '    <section class="card">\n      <div class="card__header">\n        <h2>Upcoming Picks</h2>', '    <section class="card draft-side" data-mobile-collapsible>\n      <div class="card__header">\n        <h2>Upcoming Picks</h2>')
replace_once("frontend/draft.html", '    <section class="card">\n      <div class="card__header">\n        <h2>Recommended Board</h2>', '    <section class="card draft-side" data-mobile-collapsible>\n      <div class="card__header">\n        <h2>Recommended Board</h2>')
replace_once("frontend/draft.html", '  <script src="draft.js"></script>', '  <script src="alpha-ui.js"></script>\n  <script src="draft.js"></script>')

replace_once("frontend/league.html", '  <main class="layout" style="max-width: 1080px">\n    <section class="league-tabs"', '  <main class="layout" style="max-width: 1080px">\n    <section class="league-tabs"')
replace_once("frontend/league.html", '  <main class="layout" style="max-width: 1080px">\n    <section class="card card--accent" data-league-panel="overview">', '  <main class="layout league-dashboard" style="max-width: 1080px">\n    <section class="card card--accent" data-league-panel="overview">')
replace_once("frontend/league.html", '  <script src="league.js"></script>', '  <script src="alpha-ui.js"></script>\n  <script src="league.js"></script>')

replace_once(
    "frontend/players.html",
    '      <form id="search-form" class="search">',
    '      <div id="player-catalog-meta" class="catalog-meta" aria-live="polite">\n'
    '        <div class="catalog-stat"><strong id="player-meta-count">—</strong><span class="muted small">Active players</span></div>\n'
    '        <div class="catalog-stat"><strong id="player-meta-teams">—</strong><span class="muted small">FBS teams</span></div>\n'
    '        <div class="catalog-stat"><strong id="player-meta-season">—</strong><span class="muted small">Season</span></div>\n'
    '        <div class="catalog-stat"><strong id="player-meta-updated">—</strong><span class="muted small">Last sync</span></div>\n'
    '      </div>\n'
    '      <form id="search-form" class="search">'
)
replace_once(
    "frontend/players.html",
    '      <div id="search-results" class="list">Loading current-season players…</div>',
    '      <div id="search-results" class="list">Loading current-season players…</div>\n'
    '      <div class="player-results-footer">\n'
    '        <span class="muted small" id="player-result-count">0 players shown</span>\n'
    '        <button class="button button--ghost" id="load-more-players" type="button" hidden>Load more players</button>\n'
    '      </div>'
)
replace_once("frontend/players.html", '  <script src="players.js"></script>', '  <script src="alpha-ui.js"></script>\n  <script src="players.js"></script>')

# Replace player browser with paginated catalog-aware implementation.
write("frontend/players.js", r'''const apiBase = window.CFF_API_BASE || '/api';
const allowLocalDemo = window.CFF_ALLOW_LOCAL_DEMO !== false;
const PAGE_SIZE = 50;

const searchForm = document.getElementById('search-form');
const searchInput = document.getElementById('search-input');
const positionFilter = document.getElementById('position-filter');
const searchResultsEl = document.getElementById('search-results');
const queueList = document.getElementById('queue-list');
const queueCount = document.getElementById('queue-count');
const playerDataStatus = document.getElementById('player-data-status');
const loadMorePlayers = document.getElementById('load-more-players');
const playerResultCount = document.getElementById('player-result-count');
const playerMetaCount = document.getElementById('player-meta-count');
const playerMetaTeams = document.getElementById('player-meta-teams');
const playerMetaSeason = document.getElementById('player-meta-season');
const playerMetaUpdated = document.getElementById('player-meta-updated');

let lastResults = [];
let currentOffset = 0;
let hasMore = false;
let loadingPlayers = false;

function safeText(value, fallback = '') {
  return escapeHtml(value ?? fallback);
}

function safeNumber(value, fallback = 0) {
  const numeric = Number(value);
  return Number.isFinite(numeric) ? numeric : fallback;
}

function refreshAuthState() {
  updateSharedNav('players');
}

document.getElementById('nav-logout')?.addEventListener('click', () => {
  clearSessionState();
  refreshAuthState();
  renderQueue();
});

async function fetchPlayers(term = '', position = '', offset = 0) {
  const params = new URLSearchParams({ limit: String(PAGE_SIZE), offset: String(offset) });
  if (term) params.set('query', term);
  if (position) params.set('position', position);
  const response = await fetch(`${apiBase}/players?${params.toString()}`, {
    headers: { Accept: 'application/json' },
  });
  if (!response.ok) throw new Error(`Player search failed with ${response.status}.`);
  return (await response.json()).map((player) => ({
    ...normalizePlayer(player),
    season: safeNumber(player.season, 0),
    updatedAt: player.updatedAt || '',
  }));
}

async function fetchPlayerMeta() {
  const response = await fetch(`${apiBase}/players/meta`, { headers: { Accept: 'application/json' } });
  if (!response.ok) throw new Error(`Player metadata failed with ${response.status}.`);
  return response.json();
}

function formatSyncTime(value) {
  if (!value) return 'Not synced';
  const parsed = new Date(value);
  if (Number.isNaN(parsed.getTime())) return 'Recently';
  return new Intl.DateTimeFormat(undefined, { month: 'short', day: 'numeric', hour: 'numeric', minute: '2-digit' }).format(parsed);
}

function renderPlayerMeta(meta = {}) {
  if (playerMetaCount) playerMetaCount.textContent = Number(meta.activePlayers || 0).toLocaleString();
  if (playerMetaTeams) playerMetaTeams.textContent = Number(meta.teams || 0).toLocaleString();
  if (playerMetaSeason) playerMetaSeason.textContent = meta.season || '—';
  if (playerMetaUpdated) playerMetaUpdated.textContent = formatSyncTime(meta.lastUpdated);
  if (playerDataStatus) {
    playerDataStatus.textContent = meta.status === 'ok'
      ? `${Number(meta.activePlayers || 0).toLocaleString()} active players across ${Number(meta.teams || 0).toLocaleString()} teams`
      : 'Player catalog metadata unavailable';
  }
}

async function loadPlayerMeta() {
  try {
    renderPlayerMeta(await fetchPlayerMeta());
  } catch {
    renderPlayerMeta({ status: 'unavailable' });
  }
}

async function loadPlayerPool({ append = false } = {}) {
  if (!searchResultsEl || loadingPlayers) return;
  loadingPlayers = true;
  const term = searchInput?.value.trim() || '';
  const position = positionFilter?.value || '';
  const offset = append ? currentOffset : 0;
  if (!append) searchResultsEl.textContent = term ? 'Searching current rosters...' : 'Loading current-season players...';
  if (loadMorePlayers) loadMorePlayers.disabled = true;

  try {
    const batch = await fetchPlayers(term, position, offset);
    const combined = append ? [...lastResults, ...batch] : batch;
    const unique = new Map(combined.map((player) => [player.id, player]));
    lastResults = [...unique.values()];
    currentOffset = offset + batch.length;
    hasMore = batch.length === PAGE_SIZE;
    renderSearchResults(lastResults);
    if (playerResultCount) playerResultCount.textContent = `${lastResults.length.toLocaleString()} player${lastResults.length === 1 ? '' : 's'} shown`;
    if (loadMorePlayers) {
      loadMorePlayers.hidden = !hasMore;
      loadMorePlayers.disabled = false;
    }
  } catch {
    if (allowLocalDemo && !append) {
      lastResults = applyPositionFilter(filterSamplePlayers(term), position);
      renderSearchResults(lastResults, true);
      if (playerDataStatus) playerDataStatus.textContent = 'Offline sample pool';
      if (playerResultCount) playerResultCount.textContent = `${lastResults.length} sample players shown`;
    } else if (!append) {
      lastResults = [];
      searchResultsEl.textContent = 'The current player database is temporarily unavailable.';
      if (playerDataStatus) playerDataStatus.textContent = 'Player sync unavailable';
      if (playerResultCount) playerResultCount.textContent = '0 players shown';
    }
    if (loadMorePlayers) loadMorePlayers.hidden = true;
  } finally {
    loadingPlayers = false;
  }
}

searchForm?.addEventListener('submit', async (event) => {
  event.preventDefault();
  currentOffset = 0;
  await loadPlayerPool();
});

positionFilter?.addEventListener('change', async () => {
  currentOffset = 0;
  await loadPlayerPool();
});

loadMorePlayers?.addEventListener('click', () => loadPlayerPool({ append: true }));

function applyPositionFilter(players, position) {
  if (!position) return players;
  return players.filter((player) => player.position === position);
}

function renderSearchResults(players = [], fallback = false) {
  if (!searchResultsEl) return;
  if (!players.length) {
    searchResultsEl.textContent = 'No active players matched those filters.';
    return;
  }
  const queuedIds = new Set(getQueue().map((player) => player.id));
  const notice = fallback
    ? '<div class="row"><div><strong>Offline player pool</strong><div class="muted">Showing sample players until the current roster database is reachable.</div></div></div>'
    : '';
  searchResultsEl.innerHTML = notice + players.map((player, index) => {
    const queued = queuedIds.has(player.id);
    const seasonLabel = player.season ? ` / ${safeNumber(player.season)}` : '';
    return `
      <div class="row">
        <div>
          <strong>${safeText(player.name, 'Unknown player')}</strong> - ${safeText(player.team, 'Team TBD')} (${safeText(player.position, 'FLEX')})
          <div class="muted">${safeText(player.conference, 'Conference TBD')} / ${safeText(player.class, 'Class TBD')}${seasonLabel}</div>
        </div>
        <button class="button" data-player-index="${index}" type="button" ${queued ? 'disabled' : ''}>${queued ? 'Queued' : 'Add to queue'}</button>
      </div>
    `;
  }).join('');

  searchResultsEl.querySelectorAll('[data-player-index]').forEach((button) => {
    button.addEventListener('click', async () => {
      const player = players[Number(button.dataset.playerIndex)];
      if (!player) return;
      addPlayerToQueue(player);
      try {
        await saveDraftQueueApi(getQueue());
      } catch {
        // The local queue remains available while the API is offline.
      }
      button.textContent = 'Queued';
      button.disabled = true;
      window.CFF_UI?.notify(`${player.name} added to your draft queue.`, 'success');
      renderQueue();
    });
  });
}

function renderQueue() {
  const queue = getQueue();
  if (queueCount) queueCount.textContent = String(queue.length);
  if (!queueList) return;
  if (!queue.length) {
    queueList.innerHTML = `
      <div class="row">
        <div>
          <strong>No queued players yet</strong>
          <div class="muted">Search above to build a ranked draft shortlist.</div>
        </div>
      </div>
    `;
    return;
  }
  queueList.innerHTML = queue.map((player, index) => `
    <div class="row">
      <div>
        <strong>${index + 1}. ${safeText(player.name, 'Unknown player')}</strong>
        <div class="muted">${safeText(player.team, 'Team TBD')} ${safeText(player.position, 'FLEX')} / Rank ${safeNumber(player.rank, 99)}</div>
      </div>
      <button class="button button--ghost" data-remove-index="${index}" type="button">Remove</button>
    </div>
  `).join('');
  queueList.querySelectorAll('[data-remove-index]').forEach((button) => {
    button.addEventListener('click', async () => {
      const player = getQueue()[Number(button.dataset.removeIndex)];
      if (!player) return;
      removeFromQueue(player.id);
      try {
        await saveDraftQueueApi(getQueue());
      } catch {
        // The local queue remains updated.
      }
      window.CFF_UI?.notify(`${player.name} removed from your queue.`, 'info');
      renderQueue();
      renderSearchResults(lastResults);
    });
  });
}

async function initPlayersPage() {
  await validateAuthSession();
  refreshAuthState();
  try {
    await syncLeaguesFromApi();
    await syncDraftFromApi();
  } catch {
    // Keep the local player queue available when the API is offline.
  }
  renderQueue();
  await Promise.all([loadPlayerMeta(), loadPlayerPool()]);
}

initPlayersPage();

window.addEventListener('storage', (event) => {
  if ([CFF_AUTH_KEY, CFF_QUEUE_KEY].includes(event.key)) {
    refreshAuthState();
    renderQueue();
  }
});
''')

# Scoreboard freshness display and helper tests.
replace_once(
    "frontend/scoreboard.js",
    "  function parseStartDate(value) {",
    "  function formatAge(value) {\n"
    "    const seconds = Number(value);\n"
    "    if (!Number.isFinite(seconds) || seconds < 0) return 'unknown';\n"
    "    if (seconds < 60) return `${Math.round(seconds)} sec ago`;\n"
    "    if (seconds < 3600) return `${Math.round(seconds / 60)} min ago`;\n"
    "    if (seconds < 86400) return `${Math.round(seconds / 3600)} hr ago`;\n"
    "    const days = Math.round(seconds / 86400);\n"
    "    return `${days} day${days === 1 ? '' : 's'} ago`;\n"
    "  }\n\n  function parseStartDate(value) {"
)
replace_once(
    "frontend/scoreboard.js",
    "    timeGroupKey,\n  };",
    "    timeGroupKey,\n    formatAge,\n  };"
)
replace_once(
    "frontend/scoreboard.js",
    "  const meta = document.getElementById('scoreboard-meta');\n",
    "  const meta = document.getElementById('scoreboard-meta');\n  const freshness = document.getElementById('scoreboard-freshness');\n"
)
replace_once(
    "frontend/scoreboard.js",
    "  async function loadScoreboard() {",
    "  function renderFreshness(payload = {}) {\n"
    "    if (!freshness) return;\n"
    "    const age = formatAge(payload.ageSeconds);\n"
    "    const scheduleAge = formatAge(payload.scheduleAgeSeconds);\n"
    "    const liveCount = Number(payload.liveGameCount || 0);\n"
    "    const stale = payload.fresh === false;\n"
    "    freshness.innerHTML = `<span class=\"data-freshness${stale ? ' is-stale' : ''}\">${stale ? 'Score cache delayed' : 'Score cache current'} · ${age}</span><span>${Number(payload.scheduleGameCount || 0)} scheduled games · schedule ${scheduleAge}${liveCount ? ` · ${liveCount} live` : ''}</span>`;\n"
    "  }\n\n  async function loadScoreboard() {"
)
replace_once(
    "frontend/scoreboard.js",
    "      renderLiveScores(await response.json(), false);",
    "      renderLiveScores(await response.json(), false);\n"
    "      try {\n"
    "        const metaResponse = await root.fetch(`${apiBase}/scores/live/meta`, { headers: { Accept: 'application/json' } });\n"
    "        if (metaResponse.ok) renderFreshness(await metaResponse.json());\n"
    "        else if (freshness) freshness.textContent = 'Schedule freshness unavailable';\n"
    "      } catch {\n"
    "        if (freshness) freshness.textContent = 'Schedule freshness unavailable';\n"
    "      }"
)

# Contract and live API tests.
write("scripts/alpha_ui_contract_tests.py", r'''#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def text(path):
    return (ROOT / path).read_text(encoding="utf-8")


render = text("render.yaml")
ci = text(".github/workflows/ci.yml")
main = text("backend/src/main.cpp")
player_h = text("backend/src/player_catalog.h")
player_cpp = text("backend/src/player_catalog.cpp")
live_h = text("backend/src/live_scores.h")
index = text("frontend/index.html")
players = text("frontend/players.html")
players_js = text("frontend/players.js")
draft = text("frontend/draft.html")
league = text("frontend/league.html")
alpha_css = text("frontend/alpha-ui.css")
alpha_js = text("frontend/alpha-ui.js")

assert 'schedule: "0 10 * * 2"' in render
assert "roster['schedule'] == '0 10 * * 2'" in ci
assert '"/api/players/meta"' in main
assert '"/api/scores/live/meta"' in main
assert 'getOptionalParam(req, "team")' in main
assert 'req->getParameter("offset")' in main
assert 'Json::Value playerCatalogMeta();' in player_h
assert 'std::size_t offset = 0' in player_h
assert 'OFFSET $' in player_cpp
assert 'Json::Value cachedLiveScoreMeta();' in live_h
assert 'id="scoreboard-freshness"' in index
assert 'id="load-more-players"' in players
assert 'id="player-catalog-meta"' in players
assert "params.set('offset', String(offset))" in players_js or "offset: String(offset)" in players_js
assert 'data-mobile-collapsible' in draft
assert 'draft-dashboard' in draft
assert 'league-dashboard' in league
assert 'league-tab-select' in alpha_js
assert '@media (max-width: 720px)' in alpha_css
assert (ROOT / 'docs/manual-alpha-lifecycle-test-plan.md').exists()
print('Alpha UI, data, cron, and test contracts passed')
''')

write("scripts/public_data_meta_tests.py", r'''#!/usr/bin/env python3
import json
import os
import urllib.request

BASE = os.environ.get("CFF_API_BASE_URL", "http://127.0.0.1:8080").rstrip("/")


def get(path):
    request = urllib.request.Request(BASE + path, headers={"Accept": "application/json"})
    with urllib.request.urlopen(request, timeout=20) as response:
        if response.status != 200:
            raise AssertionError(f"{path} returned {response.status}")
        return json.load(response)


players_meta = get("/api/players/meta")
assert isinstance(players_meta, dict)
assert players_meta.get("status") in {"ok", "unavailable"}
assert isinstance(players_meta.get("activePlayers", 0), int)

scores_meta = get("/api/scores/live/meta")
assert isinstance(scores_meta, dict)
assert scores_meta.get("status") in {"ok", "never", "failed", "unavailable"}
assert isinstance(scores_meta.get("gameCount", 0), int)

players = get("/api/players?limit=5&offset=0")
assert isinstance(players, list)
assert len(players) <= 5
print("public player and schedule metadata smoke tests passed")
''')

write("docs/manual-alpha-lifecycle-test-plan.md", r'''# Manual Alpha lifecycle test plan

Run this checklist against the deployed custom-domain environment before labeling the application Alpha. Use two normal accounts and one commissioner account. Record screenshots or notes for every failure.

## Environment and data

- [ ] Frontend custom domain loads over HTTPS.
- [ ] API custom domain `/health` reports `status=ok` and `database=ok`.
- [ ] Player page shows catalog count, team count, season, and last-sync time.
- [ ] Player browsing loads at least two pages with **Load more players** and does not duplicate cards.
- [ ] Player search works by name, school, position, and conference.
- [ ] Home schedule shows cache freshness and groups games by week and kickoff time.
- [ ] Week navigation, kickoff-group navigation, pause/resume, and reduced-motion behavior work.

## Account and email lifecycle

- [ ] Signup sends a verification message from the configured sender domain.
- [ ] Verification link opens the custom frontend domain and activates the account.
- [ ] Resend-verification sends one new usable link.
- [ ] Password-reset email arrives and the reset link works once.
- [ ] Login, session validation, logout, and expired-session handling work.

## League lifecycle

- [ ] Commissioner creates a league and saves settings.
- [ ] Two managers join or are invited and approved.
- [ ] Mobile league-section selector reaches every section.
- [ ] Team names, roster rules, scoring rules, waiver rules, and trade rules persist after refresh.
- [ ] Draft lobby can be opened and remains locked for unauthorized users.

## Draft lifecycle

- [ ] Draft order can be randomized/reset before the first pick.
- [ ] Player queue persists after refresh and across signed-in devices.
- [ ] Draft clock and sticky mobile on-clock bar update together.
- [ ] Snake order reverses correctly each round.
- [ ] Auto-pick/timeout behavior follows configured rules.
- [ ] Undo last pick and reset draft are commissioner-only.
- [ ] A complete draft finishes without direct database intervention.

## In-season lifecycle

- [ ] Roster slot changes persist.
- [ ] Add/drop works in free-agency mode.
- [ ] Waiver submit, cancel, priority, processing, and rejection states work.
- [ ] Trade propose, accept, reject, cancel, expiration, and approval states work.
- [ ] Season schedule generation creates expected weekly matchups.
- [ ] Score week and finalize week update matchup results and standings.
- [ ] One complete scoring week finishes without manual database changes.

## Responsive and accessibility pass

Test at 390×844, 768×1024, 1366×768, and 1920×1080.

- [ ] No horizontal page overflow.
- [ ] All controls have visible focus and at least 44px touch targets on mobile.
- [ ] Secondary draft cards can expand/collapse on mobile.
- [ ] League and draft primary actions remain visible and understandable.
- [ ] Loading, empty, stale-data, offline, unauthorized, and server-error states are readable.
- [ ] Browser console has no uncaught errors.
''')

# CI and deployed workflow coverage.
replace_once(
    ".github/workflows/ci.yml",
    "            scripts/alpha_readiness.py \\\n            scripts/api_smoke_tests.py \\",
    "            scripts/alpha_readiness.py \\\n            scripts/alpha_ui_contract_tests.py \\\n            scripts/public_data_meta_tests.py \\\n            scripts/api_smoke_tests.py \\")
replace_once(
    ".github/workflows/ci.yml",
    "      - name: Validate Render Blueprint cron configuration\n",
    "      - name: Test scoreboard and Alpha UI contracts\n"
    "        run: |\n"
    "          node scripts/scoreboard_ui_tests.js\n"
    "          python scripts/alpha_ui_contract_tests.py\n"
    "      - name: Validate Render Blueprint cron configuration\n"
)
replace_once(
    ".github/workflows/ci.yml",
    "          python scripts/api_smoke_tests.py\n          python scripts/authorization_security_tests.py",
    "          python scripts/api_smoke_tests.py\n          python scripts/authorization_security_tests.py\n          python scripts/public_data_meta_tests.py"
)

for workflow in (".github/workflows/render-validation.yml", ".github/workflows/alpha-readiness.yml"):
    content = read(workflow)
    if "python scripts/public_data_meta_tests.py" not in content:
        content = content.replace(
            "run: python scripts/api_smoke_tests.py",
            "run: |\n          python scripts/api_smoke_tests.py\n          python scripts/public_data_meta_tests.py",
            1,
        )
        write(workflow, content)

# Update JS regression tests for pagination and cache age formatting.
replace_once(
    "scripts/scoreboard_ui_tests.js",
    "  buildSlides,\n} = require('../frontend/scoreboard.js');",
    "  buildSlides,\n  formatAge,\n} = require('../frontend/scoreboard.js');"
)
replace_once(
    "scripts/scoreboard_ui_tests.js",
    "assert.ok(slides[0].games[0].startDate <= slides[1].games[0].startDate);",
    "assert.ok(slides[0].games[0].startDate <= slides[1].games[0].startDate);\n"
    "assert.equal(formatAge(45), '45 sec ago');\n"
    "assert.equal(formatAge(120), '2 min ago');\n"
    "assert.equal(formatAge(7200), '2 hr ago');"
)
old_tail = r'''assert.match(
  playerSource,
  /params\.set\('query', term \|\| '%'\);/,
  'empty player-pool browsing must send the API-required wildcard query'
);
assert.doesNotMatch(
  playerSource,
  /if \(term\) params\.set\('query', term\);/,
  'the browse request must not omit query when no search text is present'
);

console.log('scoreboard UI tests passed; player browse regression passed');'''
new_tail = r'''assert.match(
  playerSource,
  /offset: String\(offset\)/,
  'player browsing must send a stable page offset'
);
assert.match(
  playerSource,
  /loadMorePlayers\?\.addEventListener/,
  'player browsing must expose a load-more interaction'
);
assert.match(
  playerSource,
  /fetch\(`\$\{apiBase\}\/players\/meta`/,
  'player page must load public catalog metadata'
);

console.log('scoreboard UI tests passed; paginated player browse regression passed');'''
replace_once("scripts/scoreboard_ui_tests.js", old_tail, new_tail)

print("Alpha UI/data/testing patch applied")
