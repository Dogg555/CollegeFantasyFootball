#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def text(path):
    return (ROOT / path).read_text(encoding="utf-8")


render = text("render.yaml")
ci = text(".github/workflows/ci.yml")
main = text("backend/src/main.cpp")
player_h = text("backend/src/player_catalog.h")
player_cpp = text("backend/src/player_catalog.cpp")
cfbd_ingest = text("backend/src/cfbd_ingest.cpp")
live_h = text("backend/src/live_scores.h")
config = text("frontend/config.js")
index = text("frontend/index.html")
players = text("frontend/players.html")
players_js = text("frontend/players.js")
draft = text("frontend/draft.html")
league = text("frontend/league.html")
alpha_css = text("frontend/alpha-ui.css")
alpha_js = text("frontend/alpha-ui.js")
beta_css = text("frontend/beta-ui.css")
beta_js = text("frontend/beta-ui.js")
league_nav_css = text("frontend/league-nav.css")
league_nav_js = text("frontend/league-nav.js")
favicon = text("frontend/assets/favicon.svg")

assert 'schedule: "0 10 * * 2"' in render
assert "roster['schedule'] == '0 10 * * 2'" in ci
assert '"/api/players/meta"' in main
assert '"/api/scores/live/meta"' in main
assert 'getOptionalParam(req, "team")' in main
assert 'req->getParameter("offset")' in main
assert 'Json::Value playerCatalogMeta();' in player_h
assert 'std::size_t offset = 0' in player_h
assert 'OFFSET $' in player_cpp
assert 'baseUrl + "/info"' in cfbd_ingest
assert '"remainingCalls", "remaining_calls"' in cfbd_ingest
assert 'response.status_code == 429' in cfbd_ingest
assert 'cpr::Parameters{{"year", season}, {"classification", "fbs"}}' in cfbd_ingest
assert 'CFBD bulk FBS roster' in cfbd_ingest
assert 'cpr::Parameters{{"team", team.school}' not in cfbd_ingest
run_start = cfbd_ingest.index('IngestResult runCfbdIngestOnce')
assert cfbd_ingest.index('if (players.empty()) {', run_start) < cfbd_ingest.index('upsertPlayersToPostgres(', run_start)
assert 'Json::Value cachedLiveScoreMeta();' in live_h
assert 'id="scoreboard-freshness"' in index
assert 'id="load-more-players"' in players
assert 'id="player-catalog-meta"' in players
assert "params.set('offset', String(offset))" in players_js or "offset: String(offset)" in players_js
assert 'data-mobile-collapsible' in draft
assert 'draft-dashboard' in draft
assert 'league-dashboard' in league
assert 'league-tab-select' in alpha_js
assert "new Set(['league.html', 'draft.html'])" in alpha_js
assert 'assets/favicon.svg' in alpha_js
assert 'data-cff-modern' in config
assert 'assets/favicon.svg' in config
assert 'beta-ui.css' in config
assert 'beta-ui.js' in config
assert 'league-nav.css' in config
assert 'league-nav.js' in config
assert 'signup-password-confirm' in beta_js
assert 'Email verification required' in beta_js
assert 'Passwords do not match.' in beta_js
assert 'dataset.betaGroup' in beta_js
assert '.auth-benefits' in beta_css
assert '.auth-notice' in beta_css
assert '.system-status' in beta_css
assert "[data-beta-group='team']" in beta_css
assert "label: 'Competition'" in league_nav_js
assert "items: ['scoreboard', 'standings', 'draft']" in league_nav_js
assert 'league-tabs--grouped' in league_nav_js
assert 'league-nav-group__label' in league_nav_css
assert "[data-nav-group='competition']" in league_nav_css
assert '@media (max-width: 1120px)' in league_nav_css
assert '@media (max-width: 760px)' in alpha_css
assert '@media (max-width: 760px)' in beta_css
assert '<svg' in favicon and 'linearGradient' in favicon
assert (ROOT / 'docs/manual-alpha-lifecycle-test-plan.md').exists()
print('Alpha UI, beta polish, grouped league navigation, privacy, branding, data, cron, quota, and bulk-roster contracts passed')
