UPDATE league_matchups
SET identity_key = id
WHERE identity_key IS NULL OR BTRIM(identity_key) = '';

CREATE OR REPLACE FUNCTION cff_assign_matchup_identity_key()
RETURNS TRIGGER AS $$
BEGIN
  IF NEW.identity_key IS NULL OR BTRIM(NEW.identity_key) = '' THEN
    NEW.identity_key := NEW.id;
  END IF;
  RETURN NEW;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS trg_cff_assign_matchup_identity_key ON league_matchups;
CREATE TRIGGER trg_cff_assign_matchup_identity_key
BEFORE INSERT OR UPDATE OF id, identity_key ON league_matchups
FOR EACH ROW
EXECUTE FUNCTION cff_assign_matchup_identity_key();

COMMENT ON FUNCTION cff_assign_matchup_identity_key() IS
  'Keeps the migration-018 unique matchup identity compatible with schedule and scoring writers that use the stable matchup ID as the canonical identity.';

-- Replace the older finalize-only trigger with complete scoring lifecycle
-- synchronization. Scoring permanently promotes every captured lineup to a
-- scoring lock. Finalization changes the week/lineup lifecycle state without
-- changing the season schedule structure version a second time.
DROP TRIGGER IF EXISTS trg_cff_finalize_weekly_lineups ON scoring_week_states;
DROP FUNCTION IF EXISTS cff_finalize_weekly_lineups();

CREATE OR REPLACE FUNCTION cff_sync_scoring_lineup_state()
RETURNS TRIGGER AS $$
BEGIN
  IF NEW.status = 'scored' AND OLD.status IS DISTINCT FROM NEW.status THEN
    UPDATE lineup_week_states
    SET status = 'locked',
        lock_reason = 'scoring',
        version = version + 1,
        locked_at = COALESCE(locked_at, NOW()),
        unlocked_at = NULL,
        updated_at = NOW()
    WHERE league_id = NEW.league_id
      AND season = NEW.season
      AND week = NEW.week
      AND (status <> 'locked' OR lock_reason <> 'scoring');

    UPDATE schedule_week_states
    SET status = 'locked',
        version = version + 1,
        updated_at = NOW()
    WHERE league_id = NEW.league_id
      AND season = NEW.season
      AND week = NEW.week;

    UPDATE schedule_states
    SET version = version + 1,
        updated_at = NOW()
    WHERE league_id = NEW.league_id
      AND season = NEW.season;
  ELSIF NEW.status = 'final' AND OLD.status IS DISTINCT FROM NEW.status THEN
    UPDATE lineup_week_states
    SET status = 'finalized',
        lock_reason = 'scoring',
        version = version + 1,
        updated_at = NOW()
    WHERE league_id = NEW.league_id
      AND season = NEW.season
      AND week = NEW.week
      AND (status <> 'finalized' OR lock_reason <> 'scoring');

    UPDATE schedule_week_states
    SET status = 'finalized',
        version = version + 1,
        updated_at = NOW()
    WHERE league_id = NEW.league_id
      AND season = NEW.season
      AND week = NEW.week;
  END IF;
  RETURN NEW;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS trg_cff_sync_scoring_lineup_state ON scoring_week_states;
CREATE TRIGGER trg_cff_sync_scoring_lineup_state
AFTER UPDATE OF status ON scoring_week_states
FOR EACH ROW
EXECUTE FUNCTION cff_sync_scoring_lineup_state();

COMMENT ON FUNCTION cff_sync_scoring_lineup_state() IS
  'Synchronizes scored and finalized scoring weeks into durable lineup and schedule lock state.';
