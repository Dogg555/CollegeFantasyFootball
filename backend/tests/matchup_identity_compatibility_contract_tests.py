#!/usr/bin/env python3

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MIGRATION = (ROOT / "backend/db/migrations/020_matchup_identity_compatibility.sql").read_text(encoding="utf-8")
SCHEDULE_DB = (ROOT / "backend/src/schedule_lineup_hardening_db.inc").read_text(encoding="utf-8")
SCHEDULE_ADVICE = (ROOT / "backend/src/schedule_lineup_hardening_advice.inc").read_text(encoding="utf-8")
ROSTER_DB = (ROOT / "backend/src/roster_transaction_hardening_db.inc").read_text(encoding="utf-8")
SCORING = (ROOT / "backend/src/scoring_lifecycle_hardening_mutations.inc").read_text(encoding="utf-8")


def require(source: str, fragment: str) -> None:
    assert fragment in source, f"missing contract: {fragment}"


require(MIGRATION, "SET identity_key = id")
require(MIGRATION, "CREATE OR REPLACE FUNCTION cff_assign_matchup_identity_key()")
require(MIGRATION, "NEW.identity_key := NEW.id")
require(MIGRATION, "BEFORE INSERT OR UPDATE OF id, identity_key ON league_matchups")
require(SCHEDULE_DB, "INSERT INTO league_matchups")
require(SCHEDULE_DB, "schedule_input_hash")
require(SCORING, "persistScoredMatchups")
require(SCORING, "INSERT INTO league_matchups")
require(SCHEDULE_ADVICE, "init_priority(101)")
require(SCHEDULE_ADVICE, "RosterSlotGuard")
require(ROSTER_DB, "FROM schedule_week_states week_state")
require(ROSTER_DB, "week_state.status = 'locked'")
require(ROSTER_DB, "scoring.status = 'final'")
assert "status = 'final' LIMIT 1" not in ROSTER_DB, "historical final matchups must not permanently lock rosters"

print("matchup identity and roster-lock compatibility source contracts passed")
