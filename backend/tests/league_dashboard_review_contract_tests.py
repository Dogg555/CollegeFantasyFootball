#!/usr/bin/env python3
"""Regression contracts for Codex review findings on the league dashboard."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
STATE = (ROOT / "backend/src/league_dashboard_routes_state.inc").read_text(encoding="utf-8")
ACTIONS = (ROOT / "backend/src/league_dashboard_routes_actions.inc").read_text(encoding="utf-8")
RUNTIME = (ROOT / "frontend/league-dashboard-runtime.js").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


require("FROM schedule_week_states schedule" in STATE, "lineup deadlines must come from schedule_week_states")
require("LEFT JOIN lineup_week_states lineup" in STATE, "manager lineup status must join lineup_week_states")
require("lower(lineup.manager_email) = lower($2)" in STATE, "lineup state must be scoped to the actor")
require("schedule.status <> 'finalized'" in STATE, "schedule terminal state must use finalized")
require("FROM lineup_week_states WHERE league_id = $1 AND status <> 'final'" not in STATE,
        "legacy lineup deadline query must not remain")
require("ORDER BY x.season DESC" in STATE, "current matchup selection must prioritize the latest season")

require("COUNT(*) FILTER" in ACTIONS, "action-required trades must be counted before display limiting")
require("ORDER BY created_at DESC LIMIT 5" in ACTIONS, "only the preview list should be limited")
require('payload["openCount"] = openCount;' in ACTIONS, "trade payload must expose the aggregate open count")

require("markValidated(auth.email, league.id);" in RUNTIME, "successful dashboard responses must validate the scope")
require(RUNTIME.index("markValidated(auth.email, league.id);") > RUNTIME.index("await root.apiRequest"),
        "scope validation must happen after the dashboard endpoint succeeds")
require("isAuthorizationFailure(error)" in RUNTIME, "authorization failures must be handled explicitly")
require("invalidateScope(auth.email, league.id);" in RUNTIME, "authorization failures must clear scoped cache")
require("allowCached && validatedScope(auth.email, league.id)" in RUNTIME,
        "transient fallback must require prior membership validation")
require("installActiveLeagueRefresh" in RUNTIME, "same-window league switches must refresh the dashboard")
require("syncHubVisibility" in RUNTIME, "the injected overview hub must follow tab visibility")
require("requestGeneration" in RUNTIME, "stale league requests must not overwrite a newer selection")

print("league dashboard Codex review contracts passed")
