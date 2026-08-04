ALTER TABLE leagues
  ADD COLUMN IF NOT EXISTS creation_key TEXT;

CREATE UNIQUE INDEX IF NOT EXISTS uq_leagues_account_creation_key
  ON leagues (account_email, creation_key)
  WHERE creation_key IS NOT NULL AND creation_key <> '';

CREATE INDEX IF NOT EXISTS idx_league_members_capacity
  ON league_members (league_id, status)
  WHERE status IN ('invited', 'pending', 'active');
