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
