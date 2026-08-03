-- Deterministic local-only player catalog for the disposable simulation database.
-- IDs are deliberately prefixed with sim-player- so they are easy to detect and
-- must never be loaded into production.

INSERT INTO players (
  id,
  full_name,
  first_name,
  last_name,
  position,
  team,
  conference,
  year,
  season,
  active,
  last_seen_at,
  raw
)
SELECT
  'sim-player-' || to_char(number, 'FM000'),
  'Simulation Player ' || to_char(number, 'FM000'),
  'Simulation',
  'Player ' || to_char(number, 'FM000'),
  (ARRAY['QB', 'RB', 'WR', 'TE', 'K'])[((number - 1) % 5) + 1],
  'Simulation State ' || (((number - 1) % 12) + 1),
  'Simulation Conference',
  (ARRAY['FR', 'SO', 'JR', 'SR'])[((number - 1) % 4) + 1],
  2026,
  TRUE,
  NOW(),
  jsonb_build_object('source', 'local-simulator', 'simulation', true)
FROM generate_series(1, 80) AS number
ON CONFLICT (id) DO UPDATE SET
  full_name = EXCLUDED.full_name,
  first_name = EXCLUDED.first_name,
  last_name = EXCLUDED.last_name,
  position = EXCLUDED.position,
  team = EXCLUDED.team,
  conference = EXCLUDED.conference,
  year = EXCLUDED.year,
  season = EXCLUDED.season,
  active = TRUE,
  last_seen_at = NOW(),
  raw = EXCLUDED.raw;
