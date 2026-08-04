#!/usr/bin/env python3

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MIGRATION = (ROOT / "backend/db/migrations/020_matchup_identity_compatibility.sql").read_text(encoding="utf-8")
SCHEDULE_DB = (ROOT / "backend/src/schedule_lineup_hardening_db.inc").read_text(encoding="utf-8")
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

print("matchup identity compatibility source contracts passed")
