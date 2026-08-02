ALTER TABLE players ADD COLUMN IF NOT EXISTS season INTEGER;
ALTER TABLE players ADD COLUMN IF NOT EXISTS active BOOLEAN NOT NULL DEFAULT TRUE;
ALTER TABLE players ADD COLUMN IF NOT EXISTS last_seen_at TIMESTAMPTZ NOT NULL DEFAULT NOW();

CREATE TABLE IF NOT EXISTS espn_roster_progress (
    season        INTEGER NOT NULL,
    scope         TEXT NOT NULL,
    team_id       TEXT NOT NULL,
    team_name     TEXT NOT NULL,
    conference    TEXT,
    status        TEXT NOT NULL CHECK (status IN ('success', 'empty', 'failed')),
    player_count  INTEGER NOT NULL DEFAULT 0,
    error_message TEXT,
    updated_at    TIMESTAMPTZ NOT NULL DEFAULT NOW(),
    PRIMARY KEY (season, scope, team_id)
);

CREATE INDEX IF NOT EXISTS idx_espn_roster_progress_status
    ON espn_roster_progress (season, scope, status, updated_at DESC);

-- Preserve progress from any FBS rows committed before per-team checkpoints
-- existed. This prevents the first resumable run from refetching those teams.
INSERT INTO espn_roster_progress (
    season,
    scope,
    team_id,
    team_name,
    conference,
    status,
    player_count,
    error_message,
    updated_at
)
SELECT
    season,
    'division-i-fbs',
    raw->>'espnTeamId',
    COALESCE(MAX(NULLIF(team, '')), 'ESPN team ' || (raw->>'espnTeamId')),
    MAX(NULLIF(conference, '')),
    'success',
    COUNT(*)::INTEGER,
    NULL,
    NOW()
FROM players
WHERE season IS NOT NULL
  AND active = TRUE
  AND raw->>'cffSource' = 'espn'
  AND raw->>'cffScope' = 'division-i-fbs'
  AND NULLIF(raw->>'espnTeamId', '') IS NOT NULL
GROUP BY season, raw->>'espnTeamId'
ON CONFLICT (season, scope, team_id) DO NOTHING;
