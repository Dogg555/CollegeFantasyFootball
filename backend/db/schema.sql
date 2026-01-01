-- College Fantasy Football Postgres schema
-- This schema is aligned with the backend search expectations (players joined to teams)
-- and is structured for CFBD-backed ingestion with category-aware player stats.

-- Enable trigram indexes for flexible name search.
CREATE EXTENSION IF NOT EXISTS pg_trgm;

-- Teams reference core CFBD identifiers.
CREATE TABLE IF NOT EXISTS teams (
    id              BIGINT PRIMARY KEY, -- CFBD team ID
    school          TEXT NOT NULL,
    mascot          TEXT,
    abbreviation    TEXT,
    conference      TEXT,
    division        TEXT,
    color           TEXT,
    alt_color       TEXT,
    logos           JSONB,
    updated_at      TIMESTAMPTZ DEFAULT NOW()
);

CREATE INDEX IF NOT EXISTS idx_teams_conference ON teams (conference);

-- Players are keyed by CFBD player ID and linked to teams.
CREATE TABLE IF NOT EXISTS players (
    id               BIGINT PRIMARY KEY, -- CFBD player ID
    full_name        TEXT NOT NULL,
    first_name       TEXT,
    last_name        TEXT,
    position         TEXT,
    team_id          BIGINT REFERENCES teams(id) ON DELETE SET NULL,
    jersey           TEXT,
    height           INT,
    weight           INT,
    class            TEXT,
    hometown         TEXT,
    conference       TEXT,
    updated_at       TIMESTAMPTZ DEFAULT NOW()
);

CREATE INDEX IF NOT EXISTS idx_players_name_trgm ON players USING gin (full_name gin_trgm_ops);
CREATE INDEX IF NOT EXISTS idx_players_position ON players (position);
CREATE INDEX IF NOT EXISTS idx_players_conference ON players (conference);

-- Games are optional but useful for linking stats.
CREATE TABLE IF NOT EXISTS games (
    id               BIGINT PRIMARY KEY, -- CFBD game ID
    season           INT NOT NULL,
    week             INT,
    season_type      TEXT,
    start_date       TIMESTAMPTZ,
    home_team        TEXT,
    away_team        TEXT,
    home_points      INT,
    away_points      INT,
    venue            TEXT,
    neutral_site     BOOLEAN,
    conference_game  BOOLEAN,
    updated_at       TIMESTAMPTZ DEFAULT NOW()
);

-- Player stats are category-aware. Defense is reserved for future ingestion.
CREATE TABLE IF NOT EXISTS player_stats (
    player_id    BIGINT REFERENCES players(id) ON DELETE CASCADE,
    season       INT NOT NULL,
    week         INT,
    team         TEXT,
    conference   TEXT,
    category     TEXT NOT NULL CHECK (category IN ('passing', 'rushing', 'receiving', 'defense')),
    stat_name    TEXT NOT NULL,
    stat_value   NUMERIC,
    game_id      BIGINT REFERENCES games(id) ON DELETE SET NULL,
    updated_at   TIMESTAMPTZ DEFAULT NOW(),
    PRIMARY KEY (player_id, season, week, category, stat_name, game_id)
);

CREATE INDEX IF NOT EXISTS idx_player_stats_cat_week ON player_stats (category, season, week);
CREATE INDEX IF NOT EXISTS idx_player_stats_player ON player_stats (player_id);

-- Ingestion run ledger to track call counts, row counts, and status.
CREATE TABLE IF NOT EXISTS ingestion_runs (
    id            BIGSERIAL PRIMARY KEY,
    resource      TEXT NOT NULL, -- e.g., 'teams', 'players', 'stats'
    season        INT,
    week          INT,
    started_at    TIMESTAMPTZ DEFAULT NOW(),
    finished_at   TIMESTAMPTZ,
    status        TEXT, -- 'success', 'failed', 'skipped'
    call_count    INT DEFAULT 0,
    row_count     INT DEFAULT 0,
    error_message TEXT
);

CREATE INDEX IF NOT EXISTS idx_ingestion_runs_resource ON ingestion_runs (resource, season, week, started_at DESC);
