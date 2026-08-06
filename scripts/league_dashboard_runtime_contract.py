#!/usr/bin/env python3
"""Production-image runtime contract for the authoritative league dashboard."""
from __future__ import annotations

import json
import os
import time
import urllib.error
import urllib.request

import psycopg

BASE_URL = os.environ.get("CFF_API_BASE_URL", "http://127.0.0.1:8080").rstrip("/")
DB_URL = os.environ["DB_URL"]
PASSWORD = os.environ.get("CFF_CONTRACT_PASSWORD", "Dashboard-Contract-Password-77!")
RUN_KEY = os.environ.get("CFF_DASHBOARD_RUN_KEY", str(int(time.time())))
OWNER = f"dashboard-owner-{RUN_KEY}@example.test"
OUTSIDER = f"dashboard-outsider-{RUN_KEY}@example.test"
OPPONENT = f"dashboard-opponent-{RUN_KEY}@example.test"
LEAGUE_ID = f"dashboard-{RUN_KEY}"


def request(method: str, path: str, body: dict | None = None, token: str = "") -> tuple[int, dict]:
    headers = {"Accept": "application/json", "Origin": "https://frontend.example.test"}
    data = None
    if body is not None:
        headers["Content-Type"] = "application/json"
        data = json.dumps(body).encode()
    if token:
        headers["Authorization"] = f"Bearer {token}"
    req = urllib.request.Request(f"{BASE_URL}{path}", data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=20) as response:
            return response.status, json.loads(response.read().decode() or "{}")
    except urllib.error.HTTPError as error:
        raw = error.read().decode()
        return error.code, json.loads(raw or "{}")


def wait_for_api() -> None:
    for _ in range(60):
        try:
            status, payload = request("GET", "/health")
            if status == 200 and payload.get("status") == "ok" and payload.get("database") == "ok":
                return
        except Exception:
            pass
        time.sleep(1)
    raise AssertionError("API did not become healthy")


def account(email: str) -> str:
    status, _ = request("POST", "/api/auth/signup", {"email": email, "password": PASSWORD})
    assert status in (200, 201, 202, 409), (status, email)
    status, payload = request("POST", "/api/auth/login", {"email": email, "password": PASSWORD})
    assert status == 200, (status, payload)
    token = payload.get("token")
    assert token, payload
    return token


def seed() -> None:
    roster_rules = json.dumps({"qb": 1, "rb": 2, "wr": 2, "te": 1, "flex": 1, "bench": 3})
    waiver_rules = json.dumps({"mode": "waivers", "claimDeadline": "2026-09-05T15:00:00Z", "freeAgencyLocked": True})
    with psycopg.connect(DB_URL, autocommit=True) as connection:
        with connection.cursor() as cursor:
            cursor.execute(
                """
                INSERT INTO leagues (
                  id, account_email, name, team_count, scoring, draft_type, draft_date,
                  draft_lobby_open, roster_rules, waiver_rules, notes
                ) VALUES (%s, %s, %s, 4, 'ppr', 'snake', '2026-09-01T18:00:00Z', true, %s::jsonb, %s::jsonb, %s)
                """,
                (LEAGUE_ID, OWNER, "Dashboard Contract League", roster_rules, waiver_rules, "Welcome to week one."),
            )
            cursor.executemany(
                """
                INSERT INTO league_members (league_id, email, team_name, role, status, joined_at)
                VALUES (%s, %s, %s, %s, 'active', NOW())
                """,
                [
                    (LEAGUE_ID, OWNER, "Owner Team", "commissioner"),
                    (LEAGUE_ID, OPPONENT, "Opponent Team", "member"),
                ],
            )
            cursor.execute(
                "INSERT INTO draft_states (league_id, status, current_pick, draft_order) VALUES (%s, 'complete', 9, %s)",
                (LEAGUE_ID, [OWNER, OPPONENT]),
            )
            roster = [
                ("player-qb", "QB One", "QB", "qb"),
                ("player-rb", "RB One", "RB", "rb"),
                ("player-wr-1", "WR One", "WR", "wr"),
                ("player-wr-2", "WR Two", "WR", "wr"),
            ]
            for player_id, name, position, slot in roster:
                cursor.execute(
                    """
                    INSERT INTO rosters (league_id, manager_email, player_id, player_snapshot, roster_slot, acquired_via)
                    VALUES (%s, %s, %s, %s::jsonb, %s, 'draft')
                    """,
                    (LEAGUE_ID, OWNER, player_id, json.dumps({"id": player_id, "name": name, "position": position}), slot),
                )
            cursor.execute(
                """
                INSERT INTO waiver_claims (
                  id, league_id, manager_email, add_player_id, add_player_snapshot,
                  drop_player_id, priority, claim_order, status
                ) VALUES (%s, %s, %s, 'player-waiver', %s::jsonb, 'player-rb', 1, 1, 'pending')
                """,
                (f"waiver-{RUN_KEY}", LEAGUE_ID, OWNER, json.dumps({"id": "player-waiver", "name": "Waiver Target"})),
            )
            incoming_trade_id = f"trade-incoming-{RUN_KEY}"
            cursor.execute(
                """
                INSERT INTO trade_offers (
                  id, league_id, offered_by_email, offered_to_email,
                  offered_player_ids, requested_player_ids, offer_player_snapshot,
                  request_player_snapshot, target_manager, status, expires_at, created_at
                ) VALUES (%s, %s, %s, %s, ARRAY['opponent-player'], ARRAY['player-rb'],
                          %s::jsonb, %s::jsonb, %s, 'pending', NOW() + INTERVAL '2 days', NOW())
                """,
                (
                    incoming_trade_id, LEAGUE_ID, OPPONENT, OWNER,
                    json.dumps([{"id": "opponent-player", "name": "Opponent Player"}]),
                    json.dumps([{"id": "player-rb", "name": "RB One"}]),
                    OWNER,
                ),
            )
            cursor.executemany(
                """
                INSERT INTO trade_player_locks (league_id, player_id, offer_id, manager_email, lock_role)
                VALUES (%s, %s, %s, %s, %s)
                """,
                [
                    (LEAGUE_ID, "opponent-player", incoming_trade_id, OPPONENT, "offered"),
                    (LEAGUE_ID, "player-rb", incoming_trade_id, OWNER, "requested"),
                ],
            )
            for index in range(6):
                cursor.execute(
                    """
                    INSERT INTO trade_offers (
                      id, league_id, offered_by_email, offered_to_email,
                      offered_player_ids, requested_player_ids, offer_player_snapshot,
                      request_player_snapshot, target_manager, status, expires_at, created_at
                    ) VALUES (%s, %s, %s, %s, ARRAY[%s], ARRAY[%s],
                              %s::jsonb, %s::jsonb, %s, 'pending', NOW() + INTERVAL '2 days',
                              NOW() + (%s * INTERVAL '1 minute'))
                    """,
                    (
                        f"trade-outgoing-{index}-{RUN_KEY}", LEAGUE_ID, OWNER, OPPONENT,
                        f"owner-offer-{index}", f"opponent-request-{index}",
                        json.dumps([{"id": f"owner-offer-{index}", "name": f"Owner Offer {index}"}]),
                        json.dumps([{"id": f"opponent-request-{index}", "name": f"Opponent Request {index}"}]),
                        OPPONENT, index + 1,
                    ),
                )
            cursor.execute(
                """
                INSERT INTO league_matchups (
                  id, league_id, season, week, home_manager_email, away_manager_email,
                  home_score, away_score, status, identity_key
                ) VALUES (%s, %s, 2025, 15, %s, %s, 100, 90, 'final', %s)
                """,
                (f"matchup-old-{RUN_KEY}", LEAGUE_ID, OWNER, OPPONENT, f"identity-old-{RUN_KEY}"),
            )
            cursor.execute(
                """
                INSERT INTO league_matchups (
                  id, league_id, season, week, home_manager_email, away_manager_email,
                  home_score, away_score, status, identity_key
                ) VALUES (%s, %s, 2026, 1, %s, %s, 12.5, 8, 'scheduled', %s)
                """,
                (f"matchup-current-{RUN_KEY}", LEAGUE_ID, OWNER, OPPONENT, f"identity-current-{RUN_KEY}"),
            )
            cursor.executemany(
                """
                INSERT INTO league_standings (
                  league_id, season, manager_email, rank, wins, losses, ties,
                  games_played, points_for, points_against, win_pct, standings_version
                ) VALUES (%s, 2026, %s, %s, %s, %s, 0, 1, %s, %s, %s, 1)
                """,
                [
                    (LEAGUE_ID, OWNER, 1, 1, 0, 42.25, 30.5, 1.0),
                    (LEAGUE_ID, OPPONENT, 2, 0, 1, 30.5, 42.25, 0.0),
                ],
            )
            cursor.execute(
                """
                INSERT INTO schedule_week_states (league_id, season, week, version, status, lineup_deadline)
                VALUES (%s, 2026, 1, 1, 'open', '2026-09-05T16:00:00Z')
                """,
                (LEAGUE_ID,),
            )
            cursor.execute(
                """
                INSERT INTO lineup_week_states (
                  league_id, season, week, manager_email, version, status,
                  lineup_snapshot, validation_errors
                ) VALUES (%s, 2026, 1, %s, 1, 'open', '[]'::jsonb, '[]'::jsonb)
                """,
                (LEAGUE_ID, OWNER),
            )
            cursor.execute(
                "INSERT INTO transactions (id, league_id, manager_email, transaction_type, summary) VALUES (%s, %s, %s, 'Draft', 'Owner drafted QB One')",
                (f"txn-{RUN_KEY}", LEAGUE_ID, OWNER),
            )
            cursor.execute(
                "INSERT INTO league_feed_posts (id, league_id, manager_email, body) VALUES (%s, %s, %s, 'Commissioner reminder')",
                (f"post-{RUN_KEY}", LEAGUE_ID, OWNER),
            )


def main() -> None:
    wait_for_api()
    owner_token = account(OWNER)
    outsider_token = account(OUTSIDER)
    seed()

    status, payload = request("GET", f"/api/leagues/{LEAGUE_ID}/dashboard", token=owner_token)
    assert status == 200, (status, payload)
    assert payload["league"]["name"] == "Dashboard Contract League"
    assert payload["freshness"]["source"] == "api"
    assert payload["freshness"]["stale"] is False
    assert payload["freshness"]["partial"] is False, payload["freshness"]
    assert payload["nextAction"]["code"] == "fix_lineup", payload["nextAction"]
    assert payload["lineup"]["status"] == "incomplete"
    assert payload["lineup"]["warnings"]
    assert payload["lineup"]["season"] == 2026
    assert payload["lineup"]["week"] == 1
    assert payload["lineup"]["lockStatus"] == "open"
    assert payload["lineup"]["deadline"] == "2026-09-05T16:00:00Z"
    assert payload["currentMatchup"]["season"] == 2026
    assert payload["currentMatchup"]["week"] == 1
    assert payload["currentMatchup"]["opponentTeamName"] == "Opponent Team"
    assert payload["waivers"]["pendingCount"] == 1
    assert payload["trades"]["actionRequiredCount"] == 1, payload["trades"]
    assert payload["trades"]["openCount"] == 7, payload["trades"]
    assert len(payload["trades"]["items"]) == 5
    assert not any(item["actionRequired"] for item in payload["trades"]["items"]), payload["trades"]
    assert payload["standings"]["myTeam"]["rank"] == 1
    assert len(payload["activity"]) >= 2
    assert any(item["type"] == "lineup" for item in payload["deadlines"])

    status, denied = request("GET", f"/api/leagues/{LEAGUE_ID}/dashboard", token=outsider_token)
    assert status == 404, (status, denied)
    assert denied.get("code") == "league_not_found"

    status, unauthenticated = request("GET", f"/api/leagues/{LEAGUE_ID}/dashboard")
    assert status == 401, (status, unauthenticated)
    print("league dashboard production runtime contract passed")


if __name__ == "__main__":
    main()
