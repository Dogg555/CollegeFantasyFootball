CREATE TABLE IF NOT EXISTS league_feed_posts (
  id TEXT PRIMARY KEY,
  league_id TEXT NOT NULL REFERENCES leagues(id) ON DELETE CASCADE,
  manager_email TEXT NOT NULL,
  post_type TEXT NOT NULL DEFAULT 'commissioner_post',
  body TEXT NOT NULL,
  metadata JSONB NOT NULL DEFAULT '{}'::jsonb,
  created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

CREATE INDEX IF NOT EXISTS idx_league_feed_posts_league_created
  ON league_feed_posts(league_id, created_at DESC);
