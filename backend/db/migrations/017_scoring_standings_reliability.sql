CREATE TABLE IF NOT EXISTS scoring_states (
  league_id TEXT PRIMARY KEY REFERENCES leagues(id) ON DELETE CASCADE,
  version BIGINT NOT NULL DEFAULT 0,
  standings_version BIGINT NOT NULL DEFAULT 0,
  updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE TABLE IF NOT EXISTS scoring_week_states (
  league_id TEXT NOT NULL REFERENCES leagues(id) ON DELETE CASCADE,
  season INTEGER NOT NULL,
  week INTEGER NOT NULL CHECK (week >= 1),
  version BIGINT NOT NULL DEFAULT 0,
  status TEXT NOT NULL DEFAULT 'unscored'
    CHECK (status IN ('unscored', 'scored', 'final')),
  input_hash TEXT NOT NULL DEFAULT '',
  scoring_settings_snapshot JSONB NOT NULL DEFAULT '{}'::jsonb,
  lineup_snapshot JSONB NOT NULL DEFAULT '[]'::jsonb,
  player_scores_snapshot JSONB NOT NULL DEFAULT '[]'::jsonb,
  matchup_snapshot JSONB NOT NULL DEFAULT '[]'::jsonb,
  scored_at TIMESTAMPTZ,
  finalized_at TIMESTAMPTZ,
  finalized_by_email TEXT NOT NULL DEFAULT '',
  updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  PRIMARY KEY (league_id, season, week)
);

CREATE INDEX IF NOT EXISTS idx_scoring_week_states_status
  ON scoring_week_states (league_id, season, status, week);

CREATE TABLE IF NOT EXISTS scoring_operations (
  league_id TEXT NOT NULL REFERENCES leagues(id) ON DELETE CASCADE,
  actor_email TEXT NOT NULL,
  operation_key TEXT NOT NULL,
  operation_type TEXT NOT NULL,
  season INTEGER NOT NULL,
  week INTEGER NOT NULL,
  resulting_version BIGINT NOT NULL DEFAULT 0,
  resulting_standings_version BIGINT NOT NULL DEFAULT 0,
  response_payload JSONB NOT NULL DEFAULT '{}'::jsonb,
  created_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  PRIMARY KEY (league_id, actor_email, operation_key)
);

CREATE INDEX IF NOT EXISTS idx_scoring_operations_created
  ON scoring_operations (league_id, actor_email, created_at DESC);

CREATE TABLE IF NOT EXISTS league_standings (
  league_id TEXT NOT NULL REFERENCES leagues(id) ON DELETE CASCADE,
  season INTEGER NOT NULL,
  manager_email TEXT NOT NULL,
  rank INTEGER NOT NULL DEFAULT 0,
  wins INTEGER NOT NULL DEFAULT 0,
  losses INTEGER NOT NULL DEFAULT 0,
  ties INTEGER NOT NULL DEFAULT 0,
  games_played INTEGER NOT NULL DEFAULT 0,
  points_for NUMERIC NOT NULL DEFAULT 0,
  points_against NUMERIC NOT NULL DEFAULT 0,
  win_pct NUMERIC NOT NULL DEFAULT 0,
  standings_version BIGINT NOT NULL DEFAULT 0,
  updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  PRIMARY KEY (league_id, season, manager_email)
);

CREATE INDEX IF NOT EXISTS idx_league_standings_rank
  ON league_standings (league_id, season, rank, manager_email);

ALTER TABLE league_matchups
  ADD COLUMN IF NOT EXISTS season INTEGER NOT NULL DEFAULT 0;
ALTER TABLE league_matchups
  ADD COLUMN IF NOT EXISTS scoring_snapshot_hash TEXT NOT NULL DEFAULT '';
ALTER TABLE league_matchups
  ADD COLUMN IF NOT EXISTS scoring_version BIGINT NOT NULL DEFAULT 0;

ALTER TABLE fantasy_player_scores
  ADD COLUMN IF NOT EXISTS scoring_snapshot_hash TEXT NOT NULL DEFAULT '';
ALTER TABLE fantasy_player_scores
  ADD COLUMN IF NOT EXISTS scoring_version BIGINT NOT NULL DEFAULT 0;
ALTER TABLE fantasy_player_scores
  ADD COLUMN IF NOT EXISTS finalized_at TIMESTAMPTZ;

CREATE INDEX IF NOT EXISTS idx_matchups_league_season_week
  ON league_matchups (league_id, season, week, status);
CREATE INDEX IF NOT EXISTS idx_fantasy_scores_scoring_version
  ON fantasy_player_scores (league_id, season, week, scoring_version);

INSERT INTO scoring_states (league_id, version, standings_version, updated_at)
SELECT id, 0, 0, NOW() FROM leagues
ON CONFLICT (league_id) DO NOTHING;

INSERT INTO scoring_week_states (
  league_id,
  season,
  week,
  version,
  status,
  matchup_snapshot,
  finalized_at,
  updated_at
)
SELECT
  matchup.league_id,
  matchup.season,
  matchup.week,
  1,
  CASE WHEN BOOL_AND(matchup.status = 'final') THEN 'final' ELSE 'unscored' END,
  JSONB_AGG(
    JSONB_BUILD_OBJECT(
      'id', matchup.id,
      'leagueId', matchup.league_id,
      'season', matchup.season,
      'week', matchup.week,
      'homeManager', LOWER(matchup.home_manager_email),
      'awayManager', LOWER(COALESCE(matchup.away_manager_email, '')),
      'homeScore', matchup.home_score,
      'awayScore', matchup.away_score,
      'status', matchup.status,
      'finalizedAt', COALESCE(matchup.finalized_at::text, '')
    ) ORDER BY matchup.id
  ),
  MAX(matchup.finalized_at),
  NOW()
FROM league_matchups matchup
GROUP BY matchup.league_id, matchup.season, matchup.week
ON CONFLICT (league_id, season, week) DO NOTHING;
