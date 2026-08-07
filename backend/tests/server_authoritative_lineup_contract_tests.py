#!/usr/bin/env python3
"""Structural contracts for atomic server-authoritative lineup saves."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LINEUP = (ROOT / "backend/src/roster_transaction_hardening_lineup.inc").read_text(encoding="utf-8")
HARDENING = (ROOT / "backend/src/roster_transaction_hardening.cpp").read_text(encoding="utf-8")
ADVICE = (ROOT / "backend/src/roster_transaction_hardening_advice.inc").read_text(encoding="utf-8")
ROSTER = (ROOT / "backend/src/league_roster.cpp").read_text(encoding="utf-8")
FRONTEND = (ROOT / "frontend/lineup-management.js").read_text(encoding="utf-8")
CONFIG = (ROOT / "frontend/config.js").read_text(encoding="utf-8")
MIGRATION = (ROOT / "backend/db/migrations/025_server_authoritative_lineup.sql").read_text(encoding="utf-8")


def require(value: bool, message: str) -> None:
    if not value:
        raise AssertionError(message)


require('requestedAction == "lineup"' in ADVICE, "lineup saves must use the existing roster transaction endpoint")
require('roster_transaction_hardening_lineup.inc' in HARDENING, "lineup mutation must be compiled into roster hardening")
require('#include <limits>' in HARDENING, "lineup integer narrowing must include explicit range support")
require('std::numeric_limits<int>::max()' in LINEUP, "season and week must be range-checked before narrowing")
require('value.type()' in LINEUP and 'value.asUInt64()' in LINEUP,
        "season and week parsing must distinguish signed and unsigned JSON integers")
require('expectedVersionMatches(currentVersion, body, true)' in LINEUP, "lineup save must require optimistic concurrency")
require('rosterOperationReplay' in LINEUP and 'recordRosterOperation' in LINEUP, "lineup save must be idempotent")
require('lineupAssignmentErrors' in LINEUP and 'lineupAssignmentErrors' in ROSTER, "whole lineup must validate before writes")
require('invalid_lineup_assignment' in ROSTER and 'stringMember' in ROSTER,
        "malformed assignment entries must be rejected before JsonCpp string conversion")
require(LINEUP.index('lineupAssignmentErrors') < LINEUP.index('UPDATE rosters SET roster_slot'), "validation must precede roster updates")
require('rollback(connection.get())' in LINEUP, "failed lineup saves must roll back")
require("lineup_deadline IS NOT NULL AND schedule.lineup_deadline <= NOW()" in LINEUP, "deadline must lock lineup saves")
require('slotMoveAllowed' in LINEUP, "changed players must respect individual game locks")
require('lineupWeekExists' in LINEUP and 'lineup_week_unavailable' in LINEUP,
        "lineup saves must target an existing scheduled week")
require('lineup_snapshot' in LINEUP and 'lineup_week_states.version + 1' in LINEUP, "weekly canonical snapshot must be versioned")
require("mutate('lineup'" in FRONTEND, "frontend must submit one whole-lineup mutation")
require('refreshLeagueDashboard?.({ allowCached: false })' in FRONTEND, "dashboard alerts must refresh after save")
require('saving = true' in FRONTEND and 'lineup-save' in FRONTEND, "controls must expose a saving state")
require("'schedule-lineup-lifecycle.js', 'lineup-management.js'" in CONFIG,
        "lineup editor must load after schedule lifecycle and before page behavior")
require("CHECK (roster_slot IN ('qb', 'rb', 'wr', 'te', 'flex', 'k', 'def', 'bench'))" in MIGRATION,
        "database must enforce the roster slot domain")
print("server-authoritative lineup source contracts passed")
