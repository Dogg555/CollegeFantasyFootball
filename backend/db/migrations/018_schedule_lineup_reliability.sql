CREATE TABLE IF NOT EXISTS league_schedule_states (
  league_id TEXT PRIMARY KEY REFERENCES leagues(id) ON DELETE CASCADE,
  season INTEGER NOT NULL,
  version BIGINT NOT NULL DEFAULT 0,
  weeks INTEGER NOT NULL DEFAULT 0,
  schedule_hash TEXT NOT NULL DEFAULT '',
  generated_by_email TEXT NOT NULL DEFAULT '',
  generated_at TIMESTAMPTZ,
  updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

ALTER TABLE league_matchups ADD COLUMN IF NOT EXISTS season INTEGER;
ALTER TABLE league_matchups ADD COLUMN IF NOT EXISTS schedule_version BIGINT NOT NULL DEFAULT 0;
ALTER TABLE league_matchups ADD COLUMN IF NOT EXISTS identity_key TEXT NOT NULL DEFAULT '';

UPDATE league_matchups
SET season = COALESCE(season, EXTRACT(YEAR FROM NOW())::int),
    identity_key = CASE WHEN identity_key = '' THEN id ELSE identity_key END
WHERE season IS NULL OR identity_key = '';

ALTER TABLE league_matchups ALTER COLUMN season SET NOT NULL;

CREATE UNIQUE INDEX IF NOT EXISTS uq_matchup_identity_key
  ON league_matchups (league_id, season, identity_key);
CREATE INDEX IF NOT EXISTS idx_matchups_schedule_version
  ON league_matchups (league_id, season, schedule_version, week);

CREATE TABLE IF NOT EXISTS lineup_week_states (
  league_id TEXT NOT NULL REFERENCES leagues(id) ON DELETE CASCADE,
  season INTEGER NOT NULL,
  week INTEGER NOT NULL,
  version BIGINT NOT NULL DEFAULT 0,
  status TEXT NOT NULL DEFAULT 'open' CHECK (status IN ('open', 'locked', 'final')),
  lineup_deadline TIMESTAMPTZ,
  locked_at TIMESTAMPTZ,
  locked_by_email TEXT NOT NULL DEFAULT '',
  updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  PRIMARY KEY (league_id, season, week)
);

CREATE TABLE IF NOT EXISTS lineup_snapshots (
  league_id TEXT NOT NULL REFERENCES leagues(id) ON DELETE CASCADE,
  season INTEGER NOT NULL,
  week INTEGER NOT NULL,
  manager_email TEXT NOT NULL,
  lineup_version BIGINT NOT NULL,
  roster_revision BIGINT NOT NULL DEFAULT 0,
  lineup JSONB NOT NULL DEFAULT '[]'::jsonb,
  captured_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  PRIMARY KEY (league_id, season, week, manager_email, lineup_version)
);

CREATE INDEX IF NOT EXISTS idx_lineup_snapshots_latest
  ON lineup_snapshots (league_id, season, week, manager_email, lineup_version DESC);

CREATE TABLE IF NOT EXISTS schedule_lineup_operations (
  league_id TEXT NOT NULL REFERENCES leagues(id) ON DELETE CASCADE,
  actor_email TEXT NOT NULL,
  operation_key TEXT NOT NULL,
  operation_type TEXT NOT NULL,
  resulting_version BIGINT NOT NULL DEFAULT 0,
  response_payload JSONB NOT NULL DEFAULT '{}'::jsonb,
  created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  PRIMARY KEY (league_id, actor_email, operation_key)
);

CREATE INDEX IF NOT EXISTS idx_schedule_lineup_operations_created
  ON schedule_lineup_operations (league_id, actor_email, created_at DESC);
