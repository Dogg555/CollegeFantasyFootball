ALTER TABLE players ADD COLUMN IF NOT EXISTS season INTEGER;
ALTER TABLE players ADD COLUMN IF NOT EXISTS active BOOLEAN NOT NULL DEFAULT TRUE;
ALTER TABLE players ADD COLUMN IF NOT EXISTS last_seen_at TIMESTAMPTZ NOT NULL DEFAULT NOW();

CREATE INDEX IF NOT EXISTS idx_players_active_season
  ON players (active, season DESC);
CREATE INDEX IF NOT EXISTS idx_players_team_active
  ON players (team, active);

UPDATE players
SET active = TRUE,
    last_seen_at = COALESCE(last_seen_at, updated_at, NOW())
WHERE active IS DISTINCT FROM TRUE OR last_seen_at IS NULL;
