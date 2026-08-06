#!/usr/bin/env python3
"""Structural contracts for authoritative league context composition."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CONTEXT = (ROOT / "backend/src/league_context.h").read_text(encoding="utf-8")
ROUTES = (ROOT / "backend/src/league_context_routes.h").read_text(encoding="utf-8")
COMPOSITION = (ROOT / "backend/src/app_composition.cpp").read_text(encoding="utf-8")
CONFIG = (ROOT / "frontend/config.js").read_text(encoding="utf-8")
AUTHORITY = (ROOT / "frontend/league-context-authority.js").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


for field in (
    'context["leagueId"]',
    'context["leagueName"]',
    'context["userRole"]',
    'context["isCommissioner"]',
    'context["teamAssigned"]',
    'context["teamId"]',
    'context["teamName"]',
    'context["permissions"]',
    'context["serverTime"]',
):
    require(field in CONTEXT, f"league context field missing: {field}")

for permission in (
    'permissions["canEditLineup"]',
    'permissions["canAddPlayers"]',
    'permissions["canProposeTrades"]',
    'permissions["canManageLeague"]',
):
    require(permission in CONTEXT, f"league context permission missing: {permission}")

require('memberStatus == "active"' in CONTEXT, "inactive members must not receive team context")
require('memberRole == "commissioner"' in CONTEXT, "commissioner role must come from server members")
require('"/api/leagues/{1}/context"' in ROUTES, "context route is missing")
require("cff::http::requireAccount" in ROUTES, "context route must require authentication")
require("cff::handlers::handleGetLeague" in ROUTES, "context route must reuse membership-enforced league reads")
require("response->getStatusCode() != drogon::k200OK" in ROUTES, "league authorization failures must be forwarded")
require('addHeader("Cache-Control", "no-store")' in ROUTES, "context responses must not be shared-cacheable")
require('#include "league_context_routes.h"' in COMPOSITION, "application composition must include context routes")
require(
    COMPOSITION.count("cff::league_context::registerLeagueContextRoutes(") == 1,
    "application composition must register context routes exactly once",
)
require(
    COMPOSITION.index("cff::league::registerLeagueRoutes(")
    < COMPOSITION.index("cff::league_context::registerLeagueContextRoutes("),
    "context routes must be registered after the primary league route group",
)
require("'league-context.js', 'league-context-authority.js'" in CONFIG, "authority module must load after route context")
require("LEAGUE_CONTEXT_MISMATCH" in AUTHORITY, "stale cross-league mutations must be rejected")
require("TEAM_ASSIGNMENT_REQUIRED" in AUTHORITY, "unassigned teams must not mutate team workflows")
require("syncLeagueContextFromApi" in AUTHORITY, "frontend context sync API is missing")

print("league context authority contracts passed")
