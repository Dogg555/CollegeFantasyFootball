-- Canonical roster-slot domain for atomic whole-lineup saves.
UPDATE rosters
SET roster_slot = LOWER(TRIM(COALESCE(roster_slot, 'bench')));

UPDATE rosters
SET roster_slot = 'bench'
WHERE roster_slot NOT IN ('qb', 'rb', 'wr', 'te', 'flex', 'k', 'def', 'bench');

ALTER TABLE rosters
  DROP CONSTRAINT IF EXISTS rosters_roster_slot_check;

ALTER TABLE rosters
  ADD CONSTRAINT rosters_roster_slot_check
  CHECK (roster_slot IN ('qb', 'rb', 'wr', 'te', 'flex', 'k', 'def', 'bench'));

CREATE INDEX IF NOT EXISTS idx_rosters_manager_slot
  ON rosters (league_id, LOWER(manager_email), roster_slot, player_id);
