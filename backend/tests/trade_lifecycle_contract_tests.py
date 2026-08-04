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
    )
    require(
        "backend/src/trade_lifecycle_hardening_advice.inc",
        'pathLeagueId(path, "/trades/state")',
        'pathLeagueId(path, "/trades/transactions")',
        'parseTradePath(path, "/status"',
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
        "frontend/config.js",
        "trade-lifecycle.js",
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
