-- College Fantasy Football database schema
-- Vendor-agnostic tables to support multiple data providers (CFBD, Sportradar, SportsDataIO, etc.)

CREATE TABLE IF NOT EXISTS providers (
    id SERIAL PRIMARY KEY,
    name TEXT NOT NULL UNIQUE,
    description TEXT,
    created_at TIMESTAMPTZ DEFAULT now()
);

-- Teams (schools)
CREATE TABLE IF NOT EXISTS teams (
    id BIGSERIAL PRIMARY KEY,
    provider_id INT REFERENCES providers(id),
    provider_team_id TEXT,
    school TEXT NOT NULL,
    mascot TEXT,
    abbreviation TEXT,
    conference TEXT,
    division TEXT,
    location TEXT,
    colors TEXT[],
    logos TEXT[],
    active BOOLEAN DEFAULT TRUE,
    raw_payload JSONB,
    ingested_at TIMESTAMPTZ DEFAULT now(),
    UNIQUE (provider_id, provider_team_id)
);

-- Players
CREATE TABLE IF NOT EXISTS players (
    id BIGSERIAL PRIMARY KEY,
    provider_id INT REFERENCES providers(id),
    provider_player_id TEXT,
    team_id BIGINT REFERENCES teams(id),
    full_name TEXT NOT NULL,
    first_name TEXT,
    last_name TEXT,
    position TEXT,
    class TEXT,
    height_inches INT,
    weight_lbs INT,
    hometown TEXT,
    status TEXT,
    eligibility_years_remaining INT,
    raw_payload JSONB,
    ingested_at TIMESTAMPTZ DEFAULT now(),
    UNIQUE (provider_id, provider_player_id)
);

-- Games / schedule
CREATE TABLE IF NOT EXISTS games (
    id BIGSERIAL PRIMARY KEY,
    provider_id INT REFERENCES providers(id),
    provider_game_id TEXT,
    season INT NOT NULL,
    week INT NOT NULL,
    start_time TIMESTAMPTZ,
    home_team_id BIGINT REFERENCES teams(id),
    away_team_id BIGINT REFERENCES teams(id),
    venue TEXT,
    neutral_site BOOLEAN DEFAULT FALSE,
    conference_game BOOLEAN DEFAULT FALSE,
    status TEXT, -- scheduled / live / final
    home_score INT,
    away_score INT,
    raw_payload JSONB,
    ingested_at TIMESTAMPTZ DEFAULT now(),
    UNIQUE (provider_id, provider_game_id)
);

-- Player stats per game (offense/defense/special teams packed into JSONB)
CREATE TABLE IF NOT EXISTS player_game_stats (
    id BIGSERIAL PRIMARY KEY,
    game_id BIGINT REFERENCES games(id),
    player_id BIGINT REFERENCES players(id),
    team_id BIGINT REFERENCES teams(id),
    position TEXT,
    offense JSONB,
    defense JSONB,
    special_teams JSONB,
    fantasy_points_raw NUMERIC,
    fantasy_points_by_rule JSONB, -- keyed by scoring profile
    snap_counts JSONB,
    raw_payload JSONB,
    ingested_at TIMESTAMPTZ DEFAULT now(),
    UNIQUE (game_id, player_id)
);

-- Depth charts (optional but useful for lineup/injury context)
CREATE TABLE IF NOT EXISTS depth_charts (
    id BIGSERIAL PRIMARY KEY,
    team_id BIGINT REFERENCES teams(id),
    position TEXT NOT NULL,
    depth_order INT NOT NULL, -- 1/2/3
    player_id BIGINT REFERENCES players(id),
    raw_payload JSONB,
    ingested_at TIMESTAMPTZ DEFAULT now(),
    UNIQUE (team_id, position, depth_order)
);

-- Advanced team/game metrics (pace, EPA, success rate, etc.)
CREATE TABLE IF NOT EXISTS advanced_stats (
    id BIGSERIAL PRIMARY KEY,
    game_id BIGINT REFERENCES games(id),
    team_id BIGINT REFERENCES teams(id),
    metrics JSONB, -- e.g., pace, success_rate, explosiveness
    raw_payload JSONB,
    ingested_at TIMESTAMPTZ DEFAULT now(),
    UNIQUE (game_id, team_id)
);

-- Local fantasy overlays: projections, rankings, position groups, etc.
CREATE TABLE IF NOT EXISTS fantasy_profiles (
    id BIGSERIAL PRIMARY KEY,
    player_id BIGINT REFERENCES players(id) UNIQUE,
    default_position_group TEXT, -- QB/RB/WR/TE/K/DEF/ST
    bye_weeks INT[],
    injury_status TEXT,
    projection_blob JSONB,
    rankings_blob JSONB,
    last_updated TIMESTAMPTZ DEFAULT now()
);

-- Provider alias mapping for flexible crosswalks
CREATE TABLE IF NOT EXISTS provider_aliases (
    id BIGSERIAL PRIMARY KEY,
    entity_type TEXT NOT NULL, -- player / team / game
    local_id BIGINT NOT NULL,
    provider_id INT REFERENCES providers(id),
    provider_entity_id TEXT NOT NULL,
    UNIQUE (entity_type, provider_id, provider_entity_id)
);

-- Helpful indexes for common access patterns
CREATE INDEX IF NOT EXISTS idx_players_team ON players(team_id);
CREATE INDEX IF NOT EXISTS idx_players_position ON players(position);
CREATE INDEX IF NOT EXISTS idx_games_season_week ON games(season, week);
CREATE INDEX IF NOT EXISTS idx_player_game_stats_player ON player_game_stats(player_id);
CREATE INDEX IF NOT EXISTS idx_player_game_stats_game ON player_game_stats(game_id);
CREATE INDEX IF NOT EXISTS idx_depth_charts_team_position ON depth_charts(team_id, position);
