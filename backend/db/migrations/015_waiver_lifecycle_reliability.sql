ALTER TABLE waiver_claims
  DROP CONSTRAINT IF EXISTS waiver_claims_status_check;

ALTER TABLE waiver_claims
  ADD CONSTRAINT waiver_claims_status_check
  CHECK (status IN ('pending', 'processed', 'cancelled', 'failed'));

ALTER TABLE waiver_claims
  ADD COLUMN IF NOT EXISTS failure_code TEXT NOT NULL DEFAULT '';

ALTER TABLE waiver_claims
  ADD COLUMN IF NOT EXISTS resolved_by_email TEXT;

ALTER TABLE waiver_claims
  ADD COLUMN IF NOT EXISTS resolution_run_id TEXT;

ALTER TABLE waiver_claims
  ADD COLUMN IF NOT EXISTS updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW();

CREATE UNIQUE INDEX IF NOT EXISTS uq_waiver_pending_manager_player
  ON waiver_claims (league_id, lower(manager_email), add_player_id)
  WHERE status = 'pending';

CREATE INDEX IF NOT EXISTS idx_waiver_pending_processing
  ON waiver_claims (league_id, status, claim_order, created_at, id);

CREATE TABLE IF NOT EXISTS waiver_states (
  league_id TEXT PRIMARY KEY REFERENCES leagues(id) ON DELETE CASCADE,
  version BIGINT NOT NULL DEFAULT 0,
  last_processed_at TIMESTAMPTZ,
  last_processing_run_id TEXT,
  updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE TABLE IF NOT EXISTS waiver_operations (
  league_id TEXT NOT NULL REFERENCES leagues(id) ON DELETE CASCADE,
  operation_key TEXT NOT NULL,
  operation_type TEXT NOT NULL,
  resulting_version BIGINT NOT NULL DEFAULT 0,
  response_payload JSONB NOT NULL DEFAULT '{}'::jsonb,
  created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  PRIMARY KEY (league_id, operation_key)
);

CREATE INDEX IF NOT EXISTS idx_waiver_operations_created
  ON waiver_operations (league_id, created_at DESC);

INSERT INTO waiver_states (league_id)
SELECT id FROM leagues
ON CONFLICT (league_id) DO NOTHING;

INSERT INTO waiver_priorities (league_id, manager_email, priority)
SELECT league_id,
       lower(email),
       ROW_NUMBER() OVER (
         PARTITION BY league_id
         ORDER BY CASE WHEN role = 'commissioner' THEN 0 ELSE 1 END,
                  COALESCE(joined_at, created_at),
                  lower(email)
       )::int
FROM league_members
WHERE status = 'active'
ON CONFLICT (league_id, manager_email) DO NOTHING;
