ALTER TABLE ingestion_runs ADD COLUMN IF NOT EXISTS run_key TEXT;
ALTER TABLE ingestion_runs ADD COLUMN IF NOT EXISTS owner_id TEXT NOT NULL DEFAULT '';
ALTER TABLE ingestion_runs ADD COLUMN IF NOT EXISTS heartbeat_at TIMESTAMPTZ;
ALTER TABLE ingestion_runs ADD COLUMN IF NOT EXISTS lease_expires_at TIMESTAMPTZ;
ALTER TABLE ingestion_runs ADD COLUMN IF NOT EXISTS attempt INTEGER NOT NULL DEFAULT 1;
ALTER TABLE ingestion_runs ADD COLUMN IF NOT EXISTS next_retry_at TIMESTAMPTZ;
ALTER TABLE ingestion_runs ADD COLUMN IF NOT EXISTS provider_status INTEGER;
ALTER TABLE ingestion_runs ADD COLUMN IF NOT EXISTS source_revision BIGINT NOT NULL DEFAULT 0;
ALTER TABLE ingestion_runs ADD COLUMN IF NOT EXISTS metadata JSONB NOT NULL DEFAULT '{}'::jsonb;

CREATE UNIQUE INDEX IF NOT EXISTS uq_ingestion_runs_run_key
  ON ingestion_runs (run_key)
  WHERE run_key IS NOT NULL AND run_key <> '';
CREATE INDEX IF NOT EXISTS idx_ingestion_runs_resource_window
  ON ingestion_runs (resource, season, week, started_at DESC);
CREATE INDEX IF NOT EXISTS idx_ingestion_runs_active_lease
  ON ingestion_runs (resource, season, week, lease_expires_at)
  WHERE status = 'running';

CREATE TABLE IF NOT EXISTS stat_ingestion_states (
  season INTEGER NOT NULL,
  week INTEGER NOT NULL CHECK (week >= 1),
  version BIGINT NOT NULL DEFAULT 0,
  source_revision BIGINT NOT NULL DEFAULT 0,
  status TEXT NOT NULL DEFAULT 'idle'
    CHECK (status IN ('idle', 'running', 'fresh', 'partial', 'retry_wait', 'failed', 'stale')),
  active_run_id BIGINT REFERENCES ingestion_runs(id) ON DELETE SET NULL,
  last_success_at TIMESTAMPTZ,
  stale_after_seconds INTEGER NOT NULL DEFAULT 900 CHECK (stale_after_seconds >= 60),
  next_retry_at TIMESTAMPTZ,
  provider_status INTEGER,
  last_error TEXT NOT NULL DEFAULT '',
  inserted_count INTEGER NOT NULL DEFAULT 0,
  corrected_count INTEGER NOT NULL DEFAULT 0,
  unchanged_count INTEGER NOT NULL DEFAULT 0,
  updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  PRIMARY KEY (season, week)
);

CREATE TABLE IF NOT EXISTS stat_ingestion_operations (
  season INTEGER NOT NULL,
  week INTEGER NOT NULL,
  actor_id TEXT NOT NULL,
  operation_key TEXT NOT NULL,
  operation_type TEXT NOT NULL,
  resulting_version BIGINT NOT NULL DEFAULT 0,
  response_payload JSONB NOT NULL DEFAULT '{}'::jsonb,
  created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  PRIMARY KEY (season, week, actor_id, operation_key)
);

CREATE INDEX IF NOT EXISTS idx_stat_ingestion_operations_created
  ON stat_ingestion_operations (season, week, actor_id, created_at DESC);

ALTER TABLE player_stats ADD COLUMN IF NOT EXISTS source_hash TEXT NOT NULL DEFAULT '';
ALTER TABLE player_stats ADD COLUMN IF NOT EXISTS source_revision BIGINT NOT NULL DEFAULT 0;
ALTER TABLE player_stats ADD COLUMN IF NOT EXISTS ingestion_run_id BIGINT REFERENCES ingestion_runs(id) ON DELETE SET NULL;
ALTER TABLE player_stats ADD COLUMN IF NOT EXISTS corrected_at TIMESTAMPTZ;

CREATE INDEX IF NOT EXISTS idx_player_stats_source_revision
  ON player_stats (season, week, source_revision DESC);
CREATE INDEX IF NOT EXISTS idx_player_stats_source_hash
  ON player_stats (source_hash)
  WHERE source_hash <> '';

CREATE TABLE IF NOT EXISTS player_stat_revisions (
  id BIGSERIAL PRIMARY KEY,
  ingestion_run_id BIGINT NOT NULL REFERENCES ingestion_runs(id) ON DELETE CASCADE,
  player_id TEXT NOT NULL,
  season INTEGER NOT NULL,
  week INTEGER NOT NULL,
  category TEXT NOT NULL,
  stat_name TEXT NOT NULL,
  game_id BIGINT NOT NULL,
  change_type TEXT NOT NULL CHECK (change_type IN ('inserted', 'corrected')),
  previous_value NUMERIC,
  new_value NUMERIC NOT NULL,
  previous_hash TEXT NOT NULL DEFAULT '',
  source_hash TEXT NOT NULL,
  source_revision BIGINT NOT NULL,
  raw_payload JSONB NOT NULL DEFAULT '{}'::jsonb,
  created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  UNIQUE (ingestion_run_id, player_id, season, week, category, stat_name, game_id)
);

CREATE INDEX IF NOT EXISTS idx_player_stat_revisions_source
  ON player_stat_revisions (season, week, source_revision DESC);
CREATE INDEX IF NOT EXISTS idx_player_stat_revisions_player
  ON player_stat_revisions (player_id, season, week, created_at DESC);

CREATE TABLE IF NOT EXISTS scoring_recalculation_queue (
  league_id TEXT NOT NULL REFERENCES leagues(id) ON DELETE CASCADE,
  season INTEGER NOT NULL,
  week INTEGER NOT NULL,
  source_revision BIGINT NOT NULL,
  status TEXT NOT NULL
    CHECK (status IN ('pending', 'blocked_final', 'processed', 'dismissed')),
  reason TEXT NOT NULL,
  player_ids TEXT[] NOT NULL DEFAULT '{}',
  detected_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  processed_at TIMESTAMPTZ,
  updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  PRIMARY KEY (league_id, season, week)
);

CREATE INDEX IF NOT EXISTS idx_scoring_recalculation_queue_status
  ON scoring_recalculation_queue (status, detected_at DESC);

CREATE OR REPLACE FUNCTION cff_mark_stat_source_stale()
RETURNS INTEGER AS $$
DECLARE
  changed INTEGER;
BEGIN
  UPDATE stat_ingestion_states
  SET status = 'stale', updated_at = NOW()
  WHERE status IN ('fresh', 'partial')
    AND last_success_at IS NOT NULL
    AND last_success_at + make_interval(secs => stale_after_seconds) < NOW();
  GET DIAGNOSTICS changed = ROW_COUNT;
  RETURN changed;
END;
$$ LANGUAGE plpgsql;
