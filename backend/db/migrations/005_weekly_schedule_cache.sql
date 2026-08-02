ALTER TABLE live_score_cache
  ADD COLUMN IF NOT EXISTS schedule_payload JSONB NOT NULL DEFAULT '[]'::jsonb;
ALTER TABLE live_score_cache
  ADD COLUMN IF NOT EXISTS schedule_fetched_at TIMESTAMPTZ;
ALTER TABLE live_score_cache
  ADD COLUMN IF NOT EXISTS schedule_game_count INTEGER NOT NULL DEFAULT 0;
