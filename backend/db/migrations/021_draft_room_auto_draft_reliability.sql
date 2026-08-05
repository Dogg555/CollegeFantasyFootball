ALTER TABLE draft_readiness
  ADD COLUMN IF NOT EXISTS auto_draft_enabled BOOLEAN NOT NULL DEFAULT FALSE;

ALTER TABLE draft_readiness
  ADD COLUMN IF NOT EXISTS consecutive_missed_picks INTEGER NOT NULL DEFAULT 0;

ALTER TABLE draft_picks
  ADD COLUMN IF NOT EXISTS selection_source TEXT NOT NULL DEFAULT 'manual';

ALTER TABLE draft_picks
  ADD COLUMN IF NOT EXISTS idempotency_key TEXT;

CREATE TABLE IF NOT EXISTS draft_activity_log (
  id BIGSERIAL PRIMARY KEY,
  league_id TEXT NOT NULL REFERENCES leagues(id) ON DELETE CASCADE,
  manager_email TEXT,
  event_type TEXT NOT NULL,
  message TEXT NOT NULL,
  pick_number INTEGER,
  player_id TEXT,
  metadata JSONB NOT NULL DEFAULT '{}'::jsonb,
  created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE INDEX IF NOT EXISTS idx_draft_activity_log_league_created
  ON draft_activity_log (league_id, created_at DESC, id DESC);
