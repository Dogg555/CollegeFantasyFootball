#!/usr/bin/env sh
set -eu

if [ -z "${DB_URL:-}" ]; then
  echo "DB_URL is required" >&2
  exit 1
fi

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"

psql "$DB_URL" -v ON_ERROR_STOP=1 -f "$ROOT_DIR/backend/db/schema.sql" >/dev/null

psql "$DB_URL" -v ON_ERROR_STOP=1 <<'SQL'
CREATE TABLE IF NOT EXISTS schema_migrations (
  version TEXT PRIMARY KEY,
  applied_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
);
INSERT INTO schema_migrations (version)
VALUES ('001_schema_snapshot')
ON CONFLICT (version) DO NOTHING;

INSERT INTO users (email, password_hash, email_verified)
VALUES ('owner@example.test', '$2b$12$collisiontestplaceholder', true);

INSERT INTO leagues (id, account_email, name, team_count, scoring, draft_type)
VALUES ('collision-league', 'OWNER@example.test', 'Collision League', 8, 'ppr', 'snake');

INSERT INTO league_members (
  league_id, email, team_name, role, status, invited_by_email,
  joined_at, created_at, updated_at
) VALUES
  ('collision-league', 'member@example.test', 'Active Team', 'member', 'active', 'OWNER@example.test', NOW() - INTERVAL '2 days', NOW() - INTERVAL '3 days', NOW() - INTERVAL '2 days'),
  ('collision-league', 'Member@example.test', '', 'member', 'invited', 'Owner@example.test', NULL, NOW() - INTERVAL '1 day', NOW() - INTERVAL '1 day');

INSERT INTO rosters (
  league_id, manager_email, player_id, player_snapshot,
  roster_slot, acquired_via, acquired_at
) VALUES
  ('collision-league', 'member@example.test', 'player-1', '{"name":"Old"}', 'bench', 'draft', NOW() - INTERVAL '2 days'),
  ('collision-league', 'Member@example.test', 'player-1', '{"name":"New"}', 'flex', 'waiver', NOW() - INTERVAL '1 day');

INSERT INTO draft_queues (league_id, manager_email, queue, updated_at) VALUES
  ('collision-league', 'member@example.test', '[{"id":"old"}]', NOW() - INTERVAL '2 days'),
  ('collision-league', 'Member@example.test', '[{"id":"new"}]', NOW() - INTERVAL '1 day');

INSERT INTO waiver_priorities (league_id, manager_email, priority, updated_at) VALUES
  ('collision-league', 'member@example.test', 2, NOW() - INTERVAL '2 days'),
  ('collision-league', 'Member@example.test', 1, NOW() - INTERVAL '1 day');

INSERT INTO fantasy_player_scores (
  league_id, manager_email, player_id, season, week,
  fantasy_points, stats, updated_at
) VALUES
  ('collision-league', 'member@example.test', 'player-1', 2026, 1, 10, '{"source":"old"}', NOW() - INTERVAL '2 days'),
  ('collision-league', 'Member@example.test', 'player-1', 2026, 1, 20, '{"source":"new"}', NOW() - INTERVAL '1 day');
SQL

sh "$ROOT_DIR/backend/db/migrate.sh"

psql "$DB_URL" -v ON_ERROR_STOP=1 <<'SQL'
DO $$
BEGIN
  IF (SELECT COUNT(*) FROM league_members WHERE league_id = 'collision-league' AND email = 'member@example.test') <> 1 THEN
    RAISE EXCEPTION 'league member collision was not consolidated';
  END IF;
  IF (SELECT status FROM league_members WHERE league_id = 'collision-league' AND email = 'member@example.test') <> 'active' THEN
    RAISE EXCEPTION 'active league membership was not preserved';
  END IF;
  IF (SELECT COUNT(*) FROM rosters WHERE league_id = 'collision-league' AND manager_email = 'member@example.test' AND player_id = 'player-1') <> 1 THEN
    RAISE EXCEPTION 'roster collision was not consolidated';
  END IF;
  IF (SELECT player_snapshot->>'name' FROM rosters WHERE league_id = 'collision-league' AND manager_email = 'member@example.test' AND player_id = 'player-1') <> 'New' THEN
    RAISE EXCEPTION 'latest roster row was not retained';
  END IF;
  IF (SELECT COUNT(*) FROM draft_queues WHERE league_id = 'collision-league' AND manager_email = 'member@example.test') <> 1 THEN
    RAISE EXCEPTION 'draft queue collision was not consolidated';
  END IF;
  IF (SELECT queue->0->>'id' FROM draft_queues WHERE league_id = 'collision-league' AND manager_email = 'member@example.test') <> 'new' THEN
    RAISE EXCEPTION 'latest draft queue was not retained';
  END IF;
  IF (SELECT priority FROM waiver_priorities WHERE league_id = 'collision-league' AND manager_email = 'member@example.test') <> 1 THEN
    RAISE EXCEPTION 'best waiver priority was not retained';
  END IF;
  IF (SELECT COUNT(*) FROM fantasy_player_scores WHERE league_id = 'collision-league' AND manager_email = 'member@example.test' AND player_id = 'player-1' AND season = 2026 AND week = 1) <> 1 THEN
    RAISE EXCEPTION 'fantasy score collision was not consolidated';
  END IF;
  IF (SELECT fantasy_points FROM fantasy_player_scores WHERE league_id = 'collision-league' AND manager_email = 'member@example.test' AND player_id = 'player-1' AND season = 2026 AND week = 1) <> 20 THEN
    RAISE EXCEPTION 'latest fantasy score was not retained';
  END IF;
  IF (SELECT account_email FROM leagues WHERE id = 'collision-league') <> 'owner@example.test' THEN
    RAISE EXCEPTION 'league owner email was not canonicalized';
  END IF;
  IF NOT EXISTS (SELECT 1 FROM schema_migrations WHERE version = '001_identity_collision_preflight') THEN
    RAISE EXCEPTION 'collision preflight migration was not recorded';
  END IF;
  IF NOT EXISTS (SELECT 1 FROM schema_migrations WHERE version = '002_identity_and_token_hardening') THEN
    RAISE EXCEPTION 'identity hardening migration was not recorded';
  END IF;
END
$$;
SQL

echo "Legacy identity collision migration test passed"
