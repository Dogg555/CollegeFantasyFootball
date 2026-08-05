#!/usr/bin/env python3
import concurrent.futures
import os
import uuid

import psycopg

DB_URL = os.environ.get("DB_URL", "postgresql://postgres:postgres@127.0.0.1:5432/cff")
RUN_KEY = os.environ.get("CFF_LIVE_STAT_RUN_KEY", "local")
SEASON = 2026
WEEK = 1


def insert_active(run_id: str):
    try:
        with psycopg.connect(DB_URL) as conn:
            with conn.cursor() as cur:
                cur.execute(
                    """INSERT INTO stat_ingest_runs
                    (id, provider, season, week, run_key, status, started_at)
                    VALUES (%s, 'cfbd', %s, %s, %s, 'running', NOW())""",
                    (run_id, SEASON, WEEK, f"{RUN_KEY}-{run_id}"),
                )
            conn.commit()
        return "accepted"
    except psycopg.errors.UniqueViolation:
        return "duplicate"


def scalar(sql: str, params=()):
    with psycopg.connect(DB_URL) as conn:
        with conn.cursor() as cur:
            cur.execute(sql, params)
            return cur.fetchone()[0]


def main():
    with psycopg.connect(DB_URL) as conn:
        with conn.cursor() as cur:
            cur.execute("DELETE FROM scoring_refresh_queue")
            cur.execute("DELETE FROM stat_ingest_source_results")
            cur.execute("DELETE FROM stat_source_freshness")
            cur.execute("DELETE FROM ingest_operator_events")
            cur.execute("DELETE FROM stat_ingest_runs WHERE season=%s AND week=%s", (SEASON, WEEK))
        conn.commit()

    ids = [f"contract-{uuid.uuid4()}" for _ in range(2)]
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        results = list(pool.map(insert_active, ids))
    assert sorted(results) == ["accepted", "duplicate"], results
    active_id = ids[results.index("accepted")]

    with psycopg.connect(DB_URL) as conn:
        with conn.cursor() as cur:
            cur.execute(
                """INSERT INTO stat_ingest_source_results
                (run_id, source, status, rows_received, rows_changed, observed_at)
                VALUES (%s, 'games', 'succeeded', 8, 2, NOW()),
                       (%s, 'player_stats', 'failed', 0, 0, NOW())""",
                (active_id, active_id),
            )
            cur.execute(
                """UPDATE stat_ingest_runs SET status='partial', rows_changed=2,
                   error_summary='player_stats unavailable', completed_at=NOW()
                   WHERE id=%s""",
                (active_id,),
            )
            cur.execute(
                """INSERT INTO stat_source_freshness
                (provider, source, season, week, state, last_attempt_at, last_success_at,
                 last_complete_run_id, consecutive_failures)
                VALUES ('cfbd','games',%s,%s,'fresh',NOW(),NOW(),%s,0),
                       ('cfbd','player_stats',%s,%s,'partial',NOW(),NULL,NULL,1)""",
                (SEASON, WEEK, active_id, SEASON, WEEK),
            )
            cur.execute(
                """INSERT INTO stat_correction_windows (season, week, opens_at, closes_at)
                VALUES (%s,%s,NOW()-INTERVAL '1 hour',NOW()+INTERVAL '24 hours')
                ON CONFLICT (season,week) DO UPDATE SET closes_at=EXCLUDED.closes_at""",
                (SEASON, WEEK),
            )
            cur.execute("SELECT id FROM leagues ORDER BY created_at LIMIT 1")
            row = cur.fetchone()
            if row:
                league_id = row[0]
            else:
                league_id = f"contract-league-{RUN_KEY}"
                cur.execute(
                    """INSERT INTO leagues (id, account_email, name, team_count)
                    VALUES (%s,'contract@example.test','Contract League',4)""",
                    (league_id,),
                )
            for _ in range(2):
                cur.execute(
                    """INSERT INTO scoring_refresh_queue
                    (league_id, season, week, reason, source_run_id)
                    VALUES (%s,%s,%s,'stats_changed',%s)
                    ON CONFLICT DO NOTHING""",
                    (league_id, SEASON, WEEK, active_id),
                )
            cur.execute(
                """INSERT INTO ingest_operator_events
                (run_id,severity,event_type,message,metadata)
                VALUES (%s,'warning','partial_ingest','One source failed',
                        '{"failedSource":"player_stats"}'::jsonb)""",
                (active_id,),
            )
        conn.commit()

    assert scalar("SELECT COUNT(*) FROM stat_ingest_runs WHERE season=%s AND week=%s", (SEASON, WEEK)) == 1
    assert scalar("SELECT status FROM stat_ingest_runs WHERE id=%s", (active_id,)) == "partial"
    assert scalar("SELECT COUNT(*) FROM stat_ingest_source_results WHERE run_id=%s", (active_id,)) == 2
    assert scalar("SELECT COUNT(*) FROM stat_source_freshness WHERE season=%s AND week=%s", (SEASON, WEEK)) == 2
    assert scalar("SELECT COUNT(*) FROM scoring_refresh_queue WHERE season=%s AND week=%s", (SEASON, WEEK)) == 1
    assert scalar("SELECT COUNT(*) FROM ingest_operator_events WHERE run_id=%s", (active_id,)) == 1
    print("live stat refresh runtime contract passed")


if __name__ == "__main__":
    main()
