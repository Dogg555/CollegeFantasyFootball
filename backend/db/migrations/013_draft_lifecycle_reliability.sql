ALTER TABLE draft_states
  ADD COLUMN IF NOT EXISTS version BIGINT NOT NULL DEFAULT 0;

ALTER TABLE draft_states
  ADD COLUMN IF NOT EXISTS completed_at TIMESTAMPTZ;

CREATE TABLE IF NOT EXISTS draft_readiness (
  league_id TEXT NOT NULL REFERENCES leagues(id) ON DELETE CASCADE,
  manager_email TEXT NOT NULL,
  ready BOOLEAN NOT NULL DEFAULT FALSE,
  last_seen_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  PRIMARY KEY (league_id, manager_email)
);

CREATE INDEX IF NOT EXISTS idx_draft_readiness_presence
  ON draft_readiness (league_id, last_seen_at DESC);

CREATE TABLE IF NOT EXISTS draft_operations (
  league_id TEXT NOT NULL REFERENCES leagues(id) ON DELETE CASCADE,
  operation_key TEXT NOT NULL,
  operation_type TEXT NOT NULL,
  resulting_version BIGINT NOT NULL DEFAULT 0,
  created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  PRIMARY KEY (league_id, operation_key)
);

CREATE INDEX IF NOT EXISTS idx_draft_operations_created
  ON draft_operations (league_id, created_at DESC);
