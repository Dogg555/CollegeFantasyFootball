CREATE TABLE IF NOT EXISTS live_score_cache (
  id SMALLINT PRIMARY KEY CHECK (id = 1),
  payload JSONB NOT NULL DEFAULT '[]'::jsonb,
  fetched_at TIMESTAMPTZ,
  status TEXT NOT NULL DEFAULT 'never' CHECK (status IN ('never', 'ok', 'failed')),
  last_error TEXT,
  game_count INTEGER NOT NULL DEFAULT 0,
  live_game_count INTEGER NOT NULL DEFAULT 0,
  updated_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

INSERT INTO live_score_cache (id)
VALUES (1)
ON CONFLICT (id) DO NOTHING;

CREATE INDEX IF NOT EXISTS idx_ingestion_runs_scoreboard_month
  ON ingestion_runs (started_at DESC)
  WHERE resource = 'scoreboard';
