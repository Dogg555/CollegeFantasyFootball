#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BETA = ROOT / "frontend" / "beta-ui.js"
STYLES = ROOT / "frontend" / "authenticated-states.css"


def require(condition, message):
    if not condition:
        raise AssertionError(message)


beta = BETA.read_text(encoding="utf-8")
styles = STYLES.read_text(encoding="utf-8")

require("installAuthenticatedStateController" in beta,
        "authenticated pages do not share one state controller")
require("root.apiRequest = async function trackedApiRequest" in beta,
        "API requests are not routed through consistent loading and result states")
require("const readOnly = method === 'GET'" in beta,
        "read-only refreshes are not separated from mutations")
require("root.location.reload()" in beta,
        "failed refresh state does not provide a safe read-only retry")
require("No local success state was recorded" in beta,
        "mutation failures can be mistaken for successful local changes")
require("The server confirmed this update" in beta,
        "successful mutations do not expose a consistent confirmation")
require("setAttribute('aria-busy', 'true')" in beta,
        "authenticated page loading is not exposed to assistive technology")
require("new MutationObserver(queueEmptyStateScan)" in beta,
        "rendered collections are not normalized into shared empty states")

for target in (
    "league-empty",
    "league-list",
    "team-roster",
    "scoreboard-list",
    "standings-list",
    "waiver-list",
    "trade-list",
    "manager-list",
    "draft-queue",
    "roster-list",
    "draft-order-list",
    "draft-pick-list",
    "upcoming-pick-list",
    "recommended-list",
):
    require(f"'{target}'" in beta, f"missing authenticated empty-state contract for {target}")

for selector in (
    ".cff-page-state",
    ".cff-page-state--loading",
    ".cff-page-state--success",
    ".cff-page-state--error",
    ".cff-state--empty",
):
    require(selector in styles, f"missing shared state styling for {selector}")

require("prefers-reduced-motion" in styles,
        "loading animation does not respect reduced-motion preferences")
require("@media (max-width: 720px)" in styles,
        "authenticated state cards are not responsive")

print("authenticated page state contracts passed")
