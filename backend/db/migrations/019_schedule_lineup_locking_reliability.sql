DO $$
BEGIN
  IF to_regclass('public.lineup_week_states') IS NOT NULL
     AND NOT EXISTS (
       SELECT 1
       FROM information_schema.columns
       WHERE table_schema = 'public'
         AND table_name = 'lineup_week_states'
         AND column_name = 'manager_email'
     ) THEN
    ALTER TABLE lineup_week_states RENAME TO lineup_week_states_legacy_018;
    IF to_regclass('public.lineup_week_states_pkey') IS NOT NULL THEN
      ALTER INDEX lineup_week_states_pkey RENAME TO lineup_week_states_legacy_018_pkey;
    END IF;
  END IF;
END
$$;

CREATE TABLE IF NOT EXISTS schedule_states (
  league_id TEXT NOT NULL REFERENCES leagues(id) ON DELETE CASCADE,
  season INTEGER NOT NULL,
  version BIGINT NOT NULL DEFAULT 0,
  input_hash TEXT NOT NULL DEFAULT '',
  weeks INTEGER NOT NULL DEFAULT 0 CHECK (weeks >= 0),
  manager_order JSONB NOT NULL DEFAULT '[]'::jsonb,
  schedule_snapshot JSONB NOT NULL DEFAULT '[]'::jsonb,
  updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  PRIMARY KEY (league_id, season)
);

CREATE TABLE IF NOT EXISTS schedule_operations (
  league_id TEXT NOT NULL REFERENCES leagues(id) ON DELETE CASCADE,
  season INTEGER NOT NULL,
  actor_email TEXT NOT NULL,
  operation_key TEXT NOT NULL,
  operation_type TEXT NOT NULL,
  resulting_version BIGINT NOT NULL DEFAULT 0,
  response_payload JSONB NOT NULL DEFAULT '{}'::jsonb,
  created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  PRIMARY KEY (league_id, actor_email, operation_key)
);

CREATE INDEX IF NOT EXISTS idx_schedule_operations_created
  ON schedule_operations (league_id, season, actor_email, created_at DESC);

CREATE TABLE IF NOT EXISTS schedule_week_states (
  league_id TEXT NOT NULL REFERENCES leagues(id) ON DELETE CASCADE,
  season INTEGER NOT NULL,
  week INTEGER NOT NULL CHECK (week >= 1),
  version BIGINT NOT NULL DEFAULT 0,
  status TEXT NOT NULL DEFAULT 'open'
    CHECK (status IN ('open', 'locked', 'finalized')),
  lineup_deadline TIMESTAMPTZ,
  updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  PRIMARY KEY (league_id, season, week)
);

CREATE TABLE IF NOT EXISTS lineup_week_states (
  league_id TEXT NOT NULL REFERENCES leagues(id) ON DELETE CASCADE,
  season INTEGER NOT NULL,
  week INTEGER NOT NULL CHECK (week >= 1),
  manager_email TEXT NOT NULL,
  version BIGINT NOT NULL DEFAULT 0,
  status TEXT NOT NULL DEFAULT 'open'
    CHECK (status IN ('open', 'locked', 'finalized')),
  lock_reason TEXT NOT NULL DEFAULT ''
    CHECK (lock_reason IN ('', 'manual', 'deadline', 'scoring')),
  lineup_snapshot JSONB NOT NULL DEFAULT '[]'::jsonb,
  validation_errors JSONB NOT NULL DEFAULT '[]'::jsonb,
  locked_at TIMESTAMPTZ,
  unlocked_at TIMESTAMPTZ,
  updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  PRIMARY KEY (league_id, season, week, manager_email)
);

CREATE INDEX IF NOT EXISTS idx_lineup_week_states_status
  ON lineup_week_states (league_id, season, week, status, manager_email);

ALTER TABLE league_matchups
  ADD COLUMN IF NOT EXISTS schedule_version BIGINT NOT NULL DEFAULT 0;
ALTER TABLE league_matchups
  ADD COLUMN IF NOT EXISTS schedule_input_hash TEXT NOT NULL DEFAULT '';

CREATE INDEX IF NOT EXISTS idx_matchups_schedule_version
  ON league_matchups (league_id, season, schedule_version, week);

INSERT INTO schedule_states (
  league_id, season, version, input_hash, weeks, manager_order, schedule_snapshot, updated_at
)
SELECT
  matchup.league_id,
  matchup.season,
  1,
  '',
  MAX(matchup.week),
  '[]'::jsonb,
  JSONB_AGG(
    JSONB_BUILD_OBJECT(
      'id', matchup.id,
      'leagueId', matchup.league_id,
      'season', matchup.season,
      'week', matchup.week,
      'homeManager', LOWER(matchup.home_manager_email),
      'awayManager', LOWER(COALESCE(matchup.away_manager_email, '')),
      'homeScore', matchup.home_score,
      'awayScore', matchup.away_score,
      'status', matchup.status,
      'finalizedAt', COALESCE(matchup.finalized_at::text, '')
    ) ORDER BY matchup.week, matchup.id
  ),
  NOW()
FROM league_matchups matchup
GROUP BY matchup.league_id, matchup.season
ON CONFLICT (league_id, season) DO NOTHING;

INSERT INTO schedule_week_states (league_id, season, week, version, status, updated_at)
SELECT
  matchup.league_id,
  matchup.season,
  matchup.week,
  1,
  CASE WHEN BOOL_AND(matchup.status = 'final') THEN 'finalized' ELSE 'open' END,
  NOW()
FROM league_matchups matchup
GROUP BY matchup.league_id, matchup.season, matchup.week
ON CONFLICT (league_id, season, week) DO NOTHING;

CREATE OR REPLACE FUNCTION cff_enforce_locked_lineup_roster()
RETURNS TRIGGER AS $$
DECLARE
  target_league TEXT;
  target_manager TEXT;
  old_active BOOLEAN := FALSE;
  new_active BOOLEAN := FALSE;
  active_lock BOOLEAN := FALSE;
BEGIN
  IF TG_OP = 'DELETE' THEN
    target_league := OLD.league_id;
    target_manager := LOWER(OLD.manager_email);
    old_active := LOWER(COALESCE(OLD.roster_slot, 'bench')) <> 'bench';
  ELSIF TG_OP = 'INSERT' THEN
    target_league := NEW.league_id;
    target_manager := LOWER(NEW.manager_email);
    new_active := LOWER(COALESCE(NEW.roster_slot, 'bench')) <> 'bench';
  ELSE
    target_league := COALESCE(NEW.league_id, OLD.league_id);
    target_manager := LOWER(COALESCE(NEW.manager_email, OLD.manager_email));
    old_active := LOWER(COALESCE(OLD.roster_slot, 'bench')) <> 'bench';
    new_active := LOWER(COALESCE(NEW.roster_slot, 'bench')) <> 'bench';
  END IF;

  IF NOT old_active AND NOT new_active THEN
    IF TG_OP = 'DELETE' THEN RETURN OLD; END IF;
    RETURN NEW;
  END IF;

  SELECT EXISTS (
    SELECT 1
    FROM lineup_week_states lineup
    LEFT JOIN scoring_week_states scoring
      ON scoring.league_id = lineup.league_id
     AND scoring.season = lineup.season
     AND scoring.week = lineup.week
    WHERE lineup.league_id = target_league
      AND LOWER(lineup.manager_email) = target_manager
      AND lineup.status = 'locked'
      AND COALESCE(scoring.status, 'unscored') <> 'final'
  ) INTO active_lock;

  IF active_lock THEN
    RAISE EXCEPTION 'lineup_locked'
      USING ERRCODE = 'P0001',
            DETAIL = 'A starter is protected by a locked weekly lineup.';
  END IF;

  IF TG_OP = 'DELETE' THEN RETURN OLD; END IF;
  RETURN NEW;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS trg_cff_enforce_locked_lineup_roster ON rosters;
CREATE TRIGGER trg_cff_enforce_locked_lineup_roster
BEFORE INSERT OR UPDATE OR DELETE ON rosters
FOR EACH ROW
EXECUTE FUNCTION cff_enforce_locked_lineup_roster();

CREATE OR REPLACE FUNCTION cff_finalize_weekly_lineups()
RETURNS TRIGGER AS $$
BEGIN
  IF NEW.status = 'final' AND OLD.status IS DISTINCT FROM NEW.status THEN
    UPDATE lineup_week_states
    SET status = 'finalized',
        lock_reason = CASE WHEN lock_reason = '' THEN 'scoring' ELSE lock_reason END,
        updated_at = NOW()
    WHERE league_id = NEW.league_id
      AND season = NEW.season
      AND week = NEW.week;

    UPDATE schedule_week_states
    SET status = 'finalized', updated_at = NOW()
    WHERE league_id = NEW.league_id
      AND season = NEW.season
      AND week = NEW.week;
  END IF;
  RETURN NEW;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS trg_cff_finalize_weekly_lineups ON scoring_week_states;
CREATE TRIGGER trg_cff_finalize_weekly_lineups
AFTER UPDATE OF status ON scoring_week_states
FOR EACH ROW
EXECUTE FUNCTION cff_finalize_weekly_lineups();
