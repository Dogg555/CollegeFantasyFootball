#!/usr/bin/env python3
"""Static contracts for post-mutation cache invalidation and authoritative refresh."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MODULE = (ROOT / "frontend" / "mutation-consistency.js").read_text(encoding="utf-8")
CONFIG = (ROOT / "frontend" / "config.js").read_text(encoding="utf-8")
STATE = (ROOT / "frontend" / "state.js").read_text(encoding="utf-8")


def require(source: str, fragment: str, message: str) -> None:
    if fragment not in source:
        raise AssertionError(message)


require(CONFIG, "'api-client.js', 'authoritative-data.js', 'mutation-consistency.js'", "mutation consistency must load with the shared reliability layer")
require(MODULE, "const MUTATION_METHODS = new Set(['POST', 'PUT', 'PATCH', 'DELETE'])", "all mutation methods must be classified")
require(MODULE, "if (!MUTATION_METHODS.has(normalizedMethod) || !normalizedPath.startsWith('/leagues')) return null", "non-league and read requests must bypass mutation refresh")

for policy in (
    "create-league",
    "delete-league",
    "league-settings",
    "join-league",
    "league-members",
    "roster",
    "waivers",
    "trades",
    "scoring",
    "league-feed",
):
    require(MODULE, f"key: '{policy}'", f"missing mutation policy: {policy}")

require(MODULE, "key: queueOnly ? 'draft-queue' : 'draft-state'", "draft queue and draft-state policies must both be classified")

for scope in ("'leagues'", "'league'", "'draft'"):
    require(MODULE, scope, f"missing cache scope {scope}")

require(MODULE, "markScopesStale(storage, policy.scopes, mutationContext)", "cache scopes must be invalidated after a confirmed mutation")
require(MODULE, "await refreshScopes(policy.scopes, mutationContext)", "mutations must wait for an authoritative refresh")
require(MODULE, "if (policy.purgeLeagueId) purgeLeagueCaches(storage, policy.purgeLeagueId)", "deleted leagues must purge scoped browser cache")
require(MODULE, "if (context.activateLeagueId && typeof rootObject.setActiveLeague === 'function')", "create and join flows must activate the returned league before collection refresh")
require(MODULE, "await rootObject.syncLeaguesFromApi()", "league list refresh is required")
require(MODULE, "await rootObject.syncActiveLeagueCollectionsFromApi()", "active league collection refresh is required")
require(MODULE, "await rootObject.syncDraftFromApi()", "draft refresh is required")
require(MODULE, "rootObject.writeApiCacheMeta?.('draft', activeLeague.id)", "successful draft refresh must receive fresh cache metadata")
require(MODULE, "publish('refresh-failed', policy, mutationContext)", "refresh failures must be broadcast")
require(MODULE, "'Change saved; refresh incomplete'", "a committed mutation with a failed refresh must not be reported as unsaved")
require(MODULE, "return { refreshed: false, context: mutationContext, error }", "refresh failure must retain the committed mutation result")
require(MODULE, "await refreshAfterMutation(policy, result", "the request wrapper must wait for invalidation and refresh before returning")
require(MODULE, "event.key === DATA_REVISION_KEY", "other tabs must re-render after shared cache revisions")

for function_name in (
    "syncLeaguesFromApi",
    "syncActiveLeagueCollectionsFromApi",
    "syncDraftFromApi",
    "writeApiCacheMeta",
    "markApiCacheStale",
):
    require(STATE, f"function {function_name}", f"state.js must expose {function_name}")

print("mutation consistency source contracts passed")
