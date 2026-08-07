#!/usr/bin/env python3
"""Structural contracts for the authoritative trade lifecycle boundary."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def require(path: str, *needles: str) -> None:
    text = (ROOT / path).read_text(encoding="utf-8")
    for needle in needles:
        assert needle in text, f"{path} is missing required contract: {needle}"


def main() -> None:
    require(
        "backend/src/trade_lifecycle_hardening.cpp",
        'getHeader("Idempotency-Key")',
        "accountEmailForRequest",
        "trade_lifecycle_hardening_mutations.inc",
        "trade_multi_player.inc",
    )
    require(
        "backend/src/trade_lifecycle_hardening_advice.inc",
        'pathLeagueId(path, "/trades/state")',
        'pathLeagueId(path, "/trades/transactions")',
        'parseTradePath(path, "/status"',
        "getTradeStatePackage",
        "dispatchTradeTransactionPackage",
        "registerSyncAdvice(tradeLifecycleAdvice)",
    )
    require(
        "backend/src/trade_lifecycle_hardening_db.inc",
        '"trade:" + leagueId',
        "lockRosterLeague(connection, leagueId)",
        "trade_states",
        "trade_operations",
        "trade_player_locks",
        "expires_at <= NOW()",
        "FOR UPDATE",
    )
    require(
        "backend/src/trade_lifecycle_hardening_mutations.inc",
        "expectedVersionMatches",
        "acquireTradePlayerLocks",
        "releaseTradePlayerLocks",
        "executeTradeSwap",
        "DELETE FROM rosters",
        "INSERT INTO rosters",
        "advanceRosterVersion",
        "trade_state_conflict",
        "trade_ownership_changed",
        "trade_player_locked",
        "trade_roster_invalid",
    )
    require(
        "backend/src/trade_multi_player.inc",
        "offerPlayers",
        "requestPlayers",
        "validOfferPlayerPackages",
        "acquireTradePlayerLocksPackage",
        "executeTradeSwapPackage",
        "planIncomingTradePackage",
        "trade_counter_not_allowed",
        'action == "counter"',
        "jsonb_array_elements_text",
        "advanceRosterVersion",
        "tradeStatePayloadPackage",
        "trade_ownership_changed",
        "trade_roster_invalid",
        "trade_state_conflict",
    )
    require(
        "backend/db/migrations/016_trade_lifecycle_reliability.sql",
        "CREATE TABLE IF NOT EXISTS trade_states",
        "CREATE TABLE IF NOT EXISTS trade_operations",
        "CREATE TABLE IF NOT EXISTS trade_player_locks",
        "PRIMARY KEY (league_id, player_id)",
        "legacy_player_lock_conflict",
        "idx_trade_offers_open_expiration",
    )
    require(
        "frontend/trade-lifecycle.js",
        "/trades/state",
        "/trades/transactions",
        "Idempotency-Key",
        "expectedVersion",
        "cff:trade-lifecycle",
        "submitTradeOfferApi",
        "updateTradeStatusApi",
    )
    require(
        "frontend/multi-player-trades.js",
        "offerPlayers",
        "requestPlayers",
        "multiple = true",
        "Send counter",
        "action === 'counter'",
        "Idempotency-Key",
        "stopImmediatePropagation",
        "playerLockedInTrade",
        "cff_trade_lifecycle_operations",
        "packageFingerprint",
        "operationFor",
        "clearOperation",
        "ensurePlayerOptions",
        "reversedOffered",
        "counterSource",
    )
    require(
        "frontend/config.js",
        "trade-lifecycle.js",
        "multi-player-trades.js",
    )
    require(
        "scripts/multi_player_trade_runtime_contract.py",
        "phase6-1-for-2",
        "phase6-2-for-3",
        "phase6-3-for-1",
        "staleProposalRejected",
        "ownershipChangeRejected",
        "atomicCapacityRollback",
        "trade_state_conflict",
        "trade_access_required",
        "trade_recipient_required",
        "trade_counter_not_allowed",
        "trade_ownership_changed",
        "trade_roster_invalid",
    )
    require(
        "backend/CMakeLists.txt",
        "src/trade_lifecycle.cpp",
        "src/trade_lifecycle_hardening.cpp",
        "trade_lifecycle_tests",
    )
    print("trade lifecycle source contracts passed")


if __name__ == "__main__":
    main()
