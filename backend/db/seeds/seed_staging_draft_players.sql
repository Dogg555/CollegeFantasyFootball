-- Staging-only draft player seed
-- 120 deterministic draftable records for 4-team and 6-team end-to-end testing.
-- Position mix: 18 QB, 30 RB, 36 WR, 18 TE, 10 K, and 8 team defenses.
-- Fictional players; safe to rerun.

WITH programs(team, conference, ordinal) AS (
    VALUES
        ('Wyoming', 'Mountain West', 1),
        ('Colorado', 'Big 12', 2),
        ('Nebraska', 'Big Ten', 3),
        ('Kansas State', 'Big 12', 4),
        ('Utah', 'Big 12', 5),
        ('Iowa State', 'Big 12', 6),
        ('Minnesota', 'Big Ten', 7),
        ('Arizona', 'Big 12', 8),
        ('Oklahoma State', 'Big 12', 9),
        ('Wisconsin', 'Big Ten', 10),
        ('Texas Tech', 'Big 12', 11),
        ('Michigan State', 'Big Ten', 12),
        ('Baylor', 'Big 12', 13),
        ('Illinois', 'Big Ten', 14),
        ('TCU', 'Big 12', 15),
        ('Cincinnati', 'Big 12', 16),
        ('Northwestern', 'Big Ten', 17),
        ('Houston', 'Big 12', 18),
        ('Purdue', 'Big Ten', 19),
        ('UCF', 'Big 12', 20),
        ('Indiana', 'Big Ten', 21),
        ('West Virginia', 'Big 12', 22),
        ('Maryland', 'Big Ten', 23),
        ('Arizona State', 'Big 12', 24)
),
position_counts(position, player_count, height, base_weight) AS (
    VALUES
        ('QB', 18, '6-3', 215),
        ('RB', 30, '5-11', 205),
        ('WR', 36, '6-1', 195),
        ('TE', 18, '6-4', 245),
        ('K', 10, '6-0', 190),
        ('DEF', 8, '', 0)
),
generated AS (
    SELECT
        'staging-' || lower(pc.position) || '-' || lpad(series.player_number::text, 3, '0') AS id,
        CASE
            WHEN pc.position = 'DEF' THEN program.team || ' Team Defense'
            ELSE 'Staging ' || pc.position || ' Prospect ' || lpad(series.player_number::text, 3, '0')
        END AS full_name,
        CASE WHEN pc.position = 'DEF' THEN program.team ELSE 'Staging' END AS first_name,
        CASE
            WHEN pc.position = 'DEF' THEN 'Defense'
            ELSE pc.position || '-' || lpad(series.player_number::text, 3, '0')
        END AS last_name,
        pc.position,
        program.team,
        program.conference,
        CASE ((series.player_number - 1) % 4)
            WHEN 0 THEN 'Freshman'
            WHEN 1 THEN 'Sophomore'
            WHEN 2 THEN 'Junior'
            ELSE 'Senior'
        END AS class_year,
        pc.height,
        pc.base_weight + CASE WHEN pc.position = 'DEF' THEN 0 ELSE ((series.player_number - 1) % 9) END AS weight,
        series.player_number
    FROM position_counts pc
    CROSS JOIN LATERAL generate_series(1, pc.player_count) AS series(player_number)
    JOIN programs program
      ON program.ordinal = 1 + ((series.player_number - 1) % 24)
)
INSERT INTO players (
    id,
    full_name,
    first_name,
    last_name,
    position,
    team,
    conference,
    year,
    height,
    weight,
    season,
    active,
    last_seen_at,
    raw
)
SELECT
    generated.id,
    generated.full_name,
    generated.first_name,
    generated.last_name,
    generated.position,
    generated.team,
    generated.conference,
    generated.class_year,
    generated.height,
    generated.weight,
    2026,
    TRUE,
    NOW(),
    jsonb_build_object(
        'source', 'staging-seed',
        'seedVersion', 2,
        'draftable', TRUE,
        'position', generated.position,
        'team', generated.team,
        'ordinal', generated.player_number
    )
FROM generated
ON CONFLICT (id) DO UPDATE SET
    full_name = EXCLUDED.full_name,
    first_name = EXCLUDED.first_name,
    last_name = EXCLUDED.last_name,
    position = EXCLUDED.position,
    team = EXCLUDED.team,
    conference = EXCLUDED.conference,
    year = EXCLUDED.year,
    height = EXCLUDED.height,
    weight = EXCLUDED.weight,
    season = EXCLUDED.season,
    active = TRUE,
    last_seen_at = NOW(),
    raw = EXCLUDED.raw;
