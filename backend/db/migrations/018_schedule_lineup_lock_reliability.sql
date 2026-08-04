CREATE TABLE IF NOT EXISTS league_schedule_states (
  league_id TEXT PRIMARY KEY REFERENCES leagues(id) ON DELETE CASCADE,
  season INTEGER NOT NULL,
  weeks INTEGER NOT NULL DEFAULT 12 CHECK (weeks BETWEEN 1 AND 18),
  current_week INTEGER NOT NULL DEFAULT 1 CHECK (current_week >= 1),
  version BIGINT NOT NULL DEFAULT 0,
  schedule_hash TEXT NOT NULL DEFAULT '',
  generated_by_email TEXT NOT NULL DEFAULT '',
  generated_at TIMESTAMPTZ,
  updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE TABLE IF NOT EXISTS league_week_controls (
  league_id TEXT NOT NULL REFERENCES leagues(id) ON DELETE CASCADE,
  season INTEGER NOT NULL,
  week INTEGER NOT NULL CHECK (week >= 1),
  schedule_version BIGINT NOT NULL DEFAULT 0,
  lineup_version BIGINT NOT NULL DEFAULT 0,
  status TEXT NOT NULL DEFAULT 'open' CHECK (status IN ('open', 'locked', 'finalized')),
  lineup_deadline TIMESTAMPTZ,
  lineup_snapshot JSONB NOT NULL DEFAULT '[]'::jsonb,
  lineup_hash TEXT NOT NULL DEFAULT '',
  locked_at TIMESTAMPTZ,
  locked_by_email TEXT NOT NULL DEFAULT '',
  lock_reason TEXT NOT NULL DEFAULT '',
  unlocked_at TIMESTAMPTZ,
  unlocked_by_email TEXT NOT NULL DEFAULT '',
  finalized_at TIMESTAMPTZ,
  updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  PRIMARY KEY (league_id, season, week)
);

CREATE INDEX IF NOT EXISTS idx_league_week_controls_current
  ON league_week_controls (league_id, season, week, status);

CREATE TABLE IF NOT EXISTS schedule_operations (
  league_id TEXT NOT NULL REFERENCES leagues(id) ON DELETE CASCADE,
  actor_email TEXT NOT NULL,
  operation_key TEXT NOT NULL,
  operation_type TEXT NOT NULL,
  resulting_version BIGINT NOT NULL DEFAULT 0,
  response_payload JSONB NOT NULL DEFAULT '{}'::jsonb,
  created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  PRIMARY KEY (league_id, actor_email, operation_key)
);

CREATE INDEX IF NOT EXISTS idx_schedule_operations_created
  ON schedule_operations (league_id, actor_email, created_at DESC);

ALTER TABLE league_matchups ADD COLUMN IF NOT EXISTS season INTEGER NOT NULL DEFAULT 0;
ALTER TABLE league_matchups ADD COLUMN IF NOT EXISTS schedule_version BIGINT NOT NULL DEFAULT 0;
ALTER TABLE league_matchups ADD COLUMN IF NOT EXISTS matchup_key TEXT NOT NULL DEFAULT '';
ALTER TABLE league_matchups ADD COLUMN IF NOT EXISTS schedule_slot INTEGER NOT NULL DEFAULT 0;

CREATE UNIQUE INDEX IF NOT EXISTS uq_league_matchups_schedule_identity
  ON league_matchups (league_id, season, week, matchup_key)
  WHERE matchup_key <> '';
CREATE INDEX IF NOT EXISTS idx_league_matchups_season_week
  ON league_matchups (league_id, season, week, schedule_slot);

CREATE OR REPLACE FUNCTION cff_current_lineup_locked(target_league TEXT)
RETURNS BOOLEAN AS $$
DECLARE
  locked BOOLEAN;
BEGIN
  SELECT EXISTS (
    SELECT 1
    FROM league_schedule_states state
    JOIN league_week_controls control
      ON control.league_id = state.league_id
     AND control.season = state.season
     AND control.week = state.current_week
    WHERE state.league_id = target_league
      AND control.status IN ('locked', 'finalized')
  ) INTO locked;
  RETURN COALESCE(locked, FALSE);
END;
$$ LANGUAGE plpgsql STABLE;

CREATE OR REPLACE FUNCTION cff_reject_roster_change_when_lineup_locked()
RETURNS TRIGGER AS $$
DECLARE
  target_league TEXT;
BEGIN
  target_league := CASE WHEN TG_OP = 'DELETE' THEN OLD.league_id ELSE NEW.league_id END;
  IF cff_current_lineup_locked(target_league) THEN
    RAISE EXCEPTION USING
      ERRCODE = 'P0001',
      MESSAGE = 'lineup_locked',
      DETAIL = 'The current league week lineup is locked.';
  END IF;
  RETURN CASE WHEN TG_OP = 'DELETE' THEN OLD ELSE NEW END;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS trg_rosters_lineup_lock ON rosters;
CREATE TRIGGER trg_rosters_lineup_lock
BEFORE INSERT OR UPDATE OR DELETE ON rosters
FOR EACH ROW EXECUTE FUNCTION cff_reject_roster_change_when_lineup_locked();

CREATE OR REPLACE FUNCTION cff_capture_lineup_snapshot(target_league TEXT)
RETURNS JSONB AS $$
DECLARE
  snapshot JSONB;
BEGIN
  SELECT COALESCE(jsonb_agg(jsonb_build_object(
    'managerEmail', lower(manager_email),
    'playerId', player_id,
    'rosterSlot', lower(roster_slot),
    'player', player_snapshot
  ) ORDER BY lower(manager_email), lower(roster_slot), player_id), '[]'::jsonb)
  INTO snapshot
  FROM rosters
  WHERE league_id = target_league
    AND lower(roster_slot) <> 'bench';
  RETURN COALESCE(snapshot, '[]'::jsonb);
END;
$$ LANGUAGE plpgsql STABLE;

CREATE OR REPLACE FUNCTION cff_sync_week_control_from_scoring()
RETURNS TRIGGER AS $$
DECLARE
  snapshot JSONB;
  schedule_version_value BIGINT;
BEGIN
  IF NEW.status = OLD.status THEN
    RETURN NEW;
  END IF;

  SELECT COALESCE(version, 0)
  INTO schedule_version_value
  FROM league_schedule_states
  WHERE league_id = NEW.league_id;
  schedule_version_value := COALESCE(schedule_version_value, 0);

  INSERT INTO league_week_controls (
    league_id, season, week, schedule_version, status, updated_at
  ) VALUES (
    NEW.league_id, NEW.season, NEW.week, schedule_version_value, 'open', NOW()
  ) ON CONFLICT (league_id, season, week) DO NOTHING;

  IF NEW.status = 'scored' THEN
    snapshot := cff_capture_lineup_snapshot(NEW.league_id);
    UPDATE league_week_controls
    SET status = CASE WHEN status = 'finalized' THEN status ELSE 'locked' END,
        lineup_version = CASE WHEN status = 'open' THEN lineup_version + 1 ELSE lineup_version END,
        lineup_snapshot = CASE WHEN status = 'open' THEN snapshot ELSE lineup_snapshot END,
        lineup_hash = CASE WHEN status = 'open' THEN md5(snapshot::text) ELSE lineup_hash END,
        locked_at = CASE WHEN status = 'open' THEN NOW() ELSE locked_at END,
        locked_by_email = CASE WHEN status = 'open' THEN 'system:scoring' ELSE locked_by_email END,
        lock_reason = CASE WHEN status = 'open' THEN 'scoring_started' ELSE lock_reason END,
        updated_at = NOW()
    WHERE league_id = NEW.league_id AND season = NEW.season AND week = NEW.week;
  ELSIF NEW.status = 'final' THEN
    UPDATE league_week_controls
    SET status = 'finalized',
        finalized_at = COALESCE(finalized_at, NOW()),
        updated_at = NOW()
    WHERE league_id = NEW.league_id AND season = NEW.season AND week = NEW.week;

    UPDATE league_schedule_states
    SET version = version + CASE WHEN current_week < NEW.week + 1 THEN 1 ELSE 0 END,
        current_week = GREATEST(current_week, NEW.week + 1),
        updated_at = NOW()
    WHERE league_id = NEW.league_id AND season = NEW.season;
  END IF;
  RETURN NEW;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS trg_scoring_week_lineup_control ON scoring_week_states;
CREATE TRIGGER trg_scoring_week_lineup_control
AFTER UPDATE OF status ON scoring_week_states
FOR EACH ROW EXECUTE FUNCTION cff_sync_week_control_from_scoring();
