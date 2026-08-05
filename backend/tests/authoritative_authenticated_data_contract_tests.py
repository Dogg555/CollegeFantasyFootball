#!/usr/bin/env python3
"""Static contracts for backend-authoritative authenticated frontend state."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
CONFIG = (ROOT / "frontend" / "config.js").read_text(encoding="utf-8")
GUARD = (ROOT / "frontend" / "authoritative-data.js").read_text(encoding="utf-8")
STATE = (ROOT / "frontend" / "state.js").read_text(encoding="utf-8")
INDEX = (ROOT / "frontend" / "index.html").read_text(encoding="utf-8")
PLAYERS = (ROOT / "frontend" / "players.html").read_text(encoding="utf-8")
LEAGUE = (ROOT / "frontend" / "league.html").read_text(encoding="utf-8")
DRAFT = (ROOT / "frontend" / "draft.html").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


scripts_match = re.search(r"const scripts = \[(.*?)\];", CONFIG, re.DOTALL)
require(scripts_match is not None, "config.js shared script list is missing")
shared_scripts = scripts_match.group(1)
require("'authoritative-data.js'" in shared_scripts, "authoritative data guard is not globally loaded")
require(
    shared_scripts.index("'authoritative-data.js'") < shared_scripts.index("'auth-session-sync.js'"),
    "authority guard must register before the remaining shared runtime layers",
)

for page_name, source in {
    "index": INDEX,
    "players": PLAYERS,
    "league": LEAGUE,
    "draft": DRAFT,
}.items():
    require(
        source.index('<script src="config.js"></script>') < source.index('<script src="state.js"></script>'),
        f"{page_name} must load config and register the guard before state.js",
    )

required_server_functions = [
    "syncLeaguesFromApi",
    "syncActiveLeagueCollectionsFromApi",
    "syncDraftFromApi",
    "saveDraftQueueApi",
    "draftPlayerApi",
    "saveLeagueToApi",
    "removeLeagueFromApi",
    "inviteMemberApi",
    "updateMemberApi",
    "addFreeAgentApi",
    "dropPlayerApi",
    "submitWaiverClaimApi",
    "submitTradeOfferApi",
    "updateRosterSlotApi",
    "generateSeasonScheduleApi",
    "finalizeWeekApi",
]
for function_name in required_server_functions:
    require(
        f"'{function_name}'" in GUARD,
        f"{function_name} is not protected by the authoritative server-session wrapper",
    )

for local_mutation in [
    "addPlayerToQueue",
    "draftPlayer",
    "addFreeAgent",
    "dropPlayer",
    "submitWaiverClaim",
    "submitTradeOffer",
    "updateTradeStatus",
]:
    require(
        f"'{local_mutation}'" in GUARD,
        f"legacy local mutation {local_mutation} is not limited to explicit demo mode",
    )

require("No browser-only changes were made" in GUARD, "missing-session errors must explain that no local write occurred")
require("authorizedDraftTurn" in GUARD, "draft-turn authority helper is missing")
require("return Boolean(manager && auth?.email && manager === auth.email)" in GUARD,
        "draft turn must require a server-provided manager identity")
require("normalizeMembersAuthoritatively" in GUARD, "strict membership normalization is missing")
require("demo && auth?.email" in GUARD, "commissioner seeding must be limited to explicit demo mode")
require("return isDemo() ? originalAvailablePlayers() : []" in GUARD,
        "production free-agent state must not use the sample player pool")
require("return isDemo() ? original.apply(this, args) : []" in GUARD,
        "production schedules must not be generated from browser-only state")
require("await root.saveDraftQueueApi(nextQueue)" in GUARD,
        "home-page queue updates must wait for backend confirmation")
require(
    GUARD.index("await root.saveDraftQueueApi(nextQueue)") < GUARD.index("root.setQueue(nextQueue)"),
    "home-page queue cache must update only after backend confirmation",
)

# The legacy local branches remain for the explicitly enabled localhost demo,
# so the guard must cover every production API wrapper that still contains one.
legacy_fallback_functions = set(re.findall(
    r"async function (\w+Api)\([^)]*\) \{(?:(?!\n\}).)*?isLocalDemoSession\(\)",
    STATE,
    re.DOTALL,
))
guarded_functions = set(re.findall(r"'([A-Za-z0-9]+Api)'", GUARD))
missing_guards = sorted(legacy_fallback_functions - guarded_functions)
require(not missing_guards, f"legacy API fallbacks are not guarded: {missing_guards}")

print("authoritative authenticated data contracts passed")
