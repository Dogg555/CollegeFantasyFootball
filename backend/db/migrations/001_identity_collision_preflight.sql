-- Consolidate legacy case-variant manager and member keys before canonical email
-- normalization in migration 002. This migration is safe to run against already
-- normalized databases and is intentionally ordered before 002.

CREATE TEMP TABLE cff_normalized_league_members ON COMMIT DROP AS
SELECT
  league_id,
  lower(btrim(email)) AS email,
  COALESCE(MAX(NULLIF(btrim(team_name), '')), '') AS team_name,
  CASE WHEN bool_or(role = 'commissioner') THEN 'commissioner' ELSE 'member' END AS role,
  CASE
    WHEN bool_or(status = 'active') THEN 'active'
    WHEN bool_or(status = 'invited') THEN 'invited'
    ELSE 'removed'
  END AS status,
  MIN(NULLIF(lower(btrim(invited_by_email)), '')) AS invited_by_email,
  MIN(joined_at) AS joined_at,
  MIN(created_at) AS created_at,
  MAX(updated_at) AS updated_at
FROM league_members
GROUP BY league_id, lower(btrim(email));

DELETE FROM league_members;
INSERT INTO league_members (
  league_id, email, team_name, role, status, invited_by_email,
  joined_at, created_at, updated_at
)
SELECT
  league_id, email, team_name, role, status, invited_by_email,
  joined_at, created_at, updated_at
FROM cff_normalized_league_members;

CREATE TEMP TABLE cff_normalized_rosters ON COMMIT DROP AS
SELECT DISTINCT ON (league_id, lower(btrim(manager_email)), player_id)
  league_id,
  lower(btrim(manager_email)) AS manager_email,
  player_id,
  player_snapshot,
  roster_slot,
  acquired_via,
  acquired_at
FROM rosters
ORDER BY league_id, lower(btrim(manager_email)), player_id, acquired_at DESC;

DELETE FROM rosters;
INSERT INTO rosters (
  league_id, manager_email, player_id, player_snapshot,
  roster_slot, acquired_via, acquired_at
)
SELECT
  league_id, manager_email, player_id, player_snapshot,
  roster_slot, acquired_via, acquired_at
FROM cff_normalized_rosters;

CREATE TEMP TABLE cff_normalized_draft_queues ON COMMIT DROP AS
SELECT DISTINCT ON (league_id, lower(btrim(manager_email)))
  league_id,
  lower(btrim(manager_email)) AS manager_email,
  queue,
  updated_at
FROM draft_queues
ORDER BY league_id, lower(btrim(manager_email)), updated_at DESC;

DELETE FROM draft_queues;
INSERT INTO draft_queues (league_id, manager_email, queue, updated_at)
SELECT league_id, manager_email, queue, updated_at
FROM cff_normalized_draft_queues;

CREATE TEMP TABLE cff_normalized_waiver_priorities ON COMMIT DROP AS
SELECT
  league_id,
  lower(btrim(manager_email)) AS manager_email,
  MIN(priority) AS priority,
  MAX(updated_at) AS updated_at
FROM waiver_priorities
GROUP BY league_id, lower(btrim(manager_email));

DELETE FROM waiver_priorities;
INSERT INTO waiver_priorities (league_id, manager_email, priority, updated_at)
SELECT league_id, manager_email, priority, updated_at
FROM cff_normalized_waiver_priorities;

CREATE TEMP TABLE cff_normalized_fantasy_scores ON COMMIT DROP AS
SELECT DISTINCT ON (
  league_id, lower(btrim(manager_email)), player_id, season, week
)
  league_id,
  lower(btrim(manager_email)) AS manager_email,
  player_id,
  season,
  week,
  fantasy_points,
  stats,
  updated_at
FROM fantasy_player_scores
ORDER BY
  league_id, lower(btrim(manager_email)), player_id, season, week,
  updated_at DESC;

DELETE FROM fantasy_player_scores;
INSERT INTO fantasy_player_scores (
  league_id, manager_email, player_id, season, week,
  fantasy_points, stats, updated_at
)
SELECT
  league_id, manager_email, player_id, season, week,
  fantasy_points, stats, updated_at
FROM cff_normalized_fantasy_scores;
