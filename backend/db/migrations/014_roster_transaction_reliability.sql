-- Reconcile any legacy duplicate ownership before enforcing one player owner per league.
WITH ranked_ownership AS (
  SELECT ctid,
         ROW_NUMBER() OVER (
           PARTITION BY league_id, player_id
           ORDER BY acquired_at ASC, manager_email ASC
         ) AS ownership_rank
  FROM rosters
)
DELETE FROM rosters roster
USING ranked_ownership duplicate
WHERE roster.ctid = duplicate.ctid
  AND duplicate.ownership_rank > 1;

CREATE UNIQUE INDEX IF NOT EXISTS uq_rosters_league_player
  ON rosters (league_id, player_id);

CREATE TABLE IF NOT EXISTS roster_states (
  league_id TEXT NOT NULL REFERENCES leagues(id) ON DELETE CASCADE,
  manager_email TEXT NOT NULL,
  version BIGINT NOT NULL DEFAULT 0,
  updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  PRIMARY KEY (league_id, manager_email)
);

CREATE INDEX IF NOT EXISTS idx_roster_states_updated
  ON roster_states (league_id, updated_at DESC);

CREATE TABLE IF NOT EXISTS roster_operations (
  league_id TEXT NOT NULL REFERENCES leagues(id) ON DELETE CASCADE,
  manager_email TEXT NOT NULL,
  operation_key TEXT NOT NULL,
  operation_type TEXT NOT NULL,
  resulting_version BIGINT NOT NULL DEFAULT 0,
  response_payload JSONB NOT NULL DEFAULT '{}'::jsonb,
  created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  PRIMARY KEY (league_id, manager_email, operation_key)
);

CREATE INDEX IF NOT EXISTS idx_roster_operations_created
  ON roster_operations (league_id, manager_email, created_at DESC);
