CREATE TABLE IF NOT EXISTS stat_ingest_runs (
  id TEXT PRIMARY KEY,
  provider TEXT NOT NULL,
  season INTEGER NOT NULL,
  week INTEGER NOT NULL,
  run_key TEXT NOT NULL,
  status TEXT NOT NULL CHECK (status IN ('queued','running','partial','succeeded','failed','duplicate','skipped')),
  force_requested BOOLEAN NOT NULL DEFAULT FALSE,
  rows_changed INTEGER NOT NULL DEFAULT 0,
  error_summary TEXT NOT NULL DEFAULT '',
  started_at TIMESTAMPTZ,
  completed_at TIMESTAMPTZ,
  created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  UNIQUE (provider, season, week, run_key)
);

CREATE UNIQUE INDEX IF NOT EXISTS uq_stat_ingest_active_scope
  ON stat_ingest_runs (provider, season, week)
  WHERE status IN ('queued','running');

CREATE INDEX IF NOT EXISTS idx_stat_ingest_runs_scope_created
  ON stat_ingest_runs (provider, season, week, created_at DESC);

CREATE TABLE IF NOT EXISTS stat_ingest_source_results (
  run_id TEXT NOT NULL REFERENCES stat_ingest_runs(id) ON DELETE CASCADE,
  source TEXT NOT NULL,
  status TEXT NOT NULL CHECK (status IN ('succeeded','failed','skipped')),
  rows_received INTEGER NOT NULL DEFAULT 0,
  rows_changed INTEGER NOT NULL DEFAULT 0,
  error_message TEXT NOT NULL DEFAULT '',
  observed_at TIMESTAMPTZ,
  completed_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  PRIMARY KEY (run_id, source)
);

CREATE TABLE IF NOT EXISTS stat_source_freshness (
  provider TEXT NOT NULL,
  source TEXT NOT NULL,
  season INTEGER NOT NULL,
  week INTEGER NOT NULL,
  state TEXT NOT NULL DEFAULT 'unknown' CHECK (state IN ('fresh','stale','partial','unavailable','unknown')),
  last_attempt_at TIMESTAMPTZ,
  last_success_at TIMESTAMPTZ,
  last_complete_run_id TEXT REFERENCES stat_ingest_runs(id) ON DELETE SET NULL,
  consecutive_failures INTEGER NOT NULL DEFAULT 0,
  updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  PRIMARY KEY (provider, source, season, week)
);

CREATE TABLE IF NOT EXISTS scoring_refresh_queue (
  id BIGSERIAL PRIMARY KEY,
  league_id TEXT NOT NULL REFERENCES leagues(id) ON DELETE CASCADE,
  season INTEGER NOT NULL,
  week INTEGER NOT NULL,
  reason TEXT NOT NULL,
  source_run_id TEXT REFERENCES stat_ingest_runs(id) ON DELETE SET NULL,
  status TEXT NOT NULL DEFAULT 'pending' CHECK (status IN ('pending','running','completed','failed','cancelled')),
  attempts INTEGER NOT NULL DEFAULT 0,
  available_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  locked_at TIMESTAMPTZ,
  completed_at TIMESTAMPTZ,
  last_error TEXT NOT NULL DEFAULT '',
  created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE UNIQUE INDEX IF NOT EXISTS uq_scoring_refresh_pending_scope
  ON scoring_refresh_queue (league_id, season, week)
  WHERE status IN ('pending','running');

CREATE INDEX IF NOT EXISTS idx_scoring_refresh_claim
  ON scoring_refresh_queue (status, available_at, created_at);

CREATE TABLE IF NOT EXISTS stat_correction_windows (
  season INTEGER NOT NULL,
  week INTEGER NOT NULL,
  opens_at TIMESTAMPTZ NOT NULL,
  closes_at TIMESTAMPTZ NOT NULL,
  closed_reason TEXT NOT NULL DEFAULT '',
  updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  PRIMARY KEY (season, week),
  CHECK (closes_at > opens_at)
);

CREATE TABLE IF NOT EXISTS ingest_operator_events (
  id BIGSERIAL PRIMARY KEY,
  run_id TEXT REFERENCES stat_ingest_runs(id) ON DELETE SET NULL,
  severity TEXT NOT NULL CHECK (severity IN ('info','warning','error')),
  event_type TEXT NOT NULL,
  message TEXT NOT NULL,
  metadata JSONB NOT NULL DEFAULT '{}'::jsonb,
  created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE INDEX IF NOT EXISTS idx_ingest_operator_events_created
  ON ingest_operator_events (created_at DESC);
