#!/usr/bin/env python3
"""PostgreSQL contracts proving fantasy mutations commit all-or-nothing."""
from __future__ import annotations

import os
import time
from collections.abc import Callable

import psycopg
from psycopg import Connection
from psycopg.errors import CheckViolation

DB_URL = os.environ["DB_URL"]
RUN_KEY = os.getenv("CFF_ATOMIC_RUN_KEY", str(time.time_ns()))


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def scalar(connection: Connection, query: str, parameters: tuple = ()):
    with connection.cursor() as cursor:
        cursor.execute(query, parameters)
        row = cursor.fetchone()
        return row[0] if row else None


def expect_atomic_rejection(
    connection: Connection,
    label: str,
    mutation: Callable[[Connection], None],
) -> None:
    try:
        with connection.transaction():
            mutation(connection)
            connection.execute("SET CONSTRAINTS ALL IMMEDIATE")
    except CheckViolation:
        return
    raise AssertionError(f"{label} unexpectedly committed an incomplete mutation")


def create_league(connection: Connection, league_id: str, managers: list[str]) -> None:
    owner = managers[0]
    connection.execute(
        """
        INSERT INTO leagues
          (id, account_email, name, team_count, scoring, draft_type, roster_rules)
        VALUES
          (%s, %s, %s, 4, 'ppr', 'snake',
           '{"qb":1,"rb":2,"wr":2,"te":1,"flex":2,"bench":6}'::jsonb)
        """,
        (league_id, owner, f"Atomic contract {league_id}"),
    )
    for index, manager in enumerate(managers):
        connection.execute(
            """
            INSERT INTO league_members
              (league_id, email, team_name, role, status, joined_at)
            VALUES (%s, %s, %s, %s, 'active', NOW())
            """,
            (
                league_id,
                manager,
                f"Atomic Team {index + 1}",
                "commissioner" if index == 0 else "member",
            ),
        )


def roster_owner(connection: Connection, league_id: str, player_id: str) -> str:
    return str(
        scalar(
            connection,
            "SELECT lower(manager_email) FROM rosters WHERE league_id = %s AND player_id = %s",
            (league_id, player_id),
        )
        or ""
    )


def draft_contract(connection: Connection) -> None:
    league_id = f"atomic-draft-{RUN_KEY}"
    manager = f"draft-{RUN_KEY}@example.test"
    pick_id = f"draft-pick-{RUN_KEY}"
    player_id = f"draft-player-{RUN_KEY}"

    with connection.transaction():
        create_league(connection, league_id, [manager])
        connection.execute(
            "INSERT INTO draft_states (league_id, status, current_pick, draft_order, version) "
            "VALUES (%s, 'open', 1, ARRAY[%s], 0)",
            (league_id, manager),
        )

    def incomplete_pick(conn: Connection) -> None:
        conn.execute(
            "UPDATE draft_states SET current_pick = 2, version = 1 WHERE league_id = %s",
            (league_id,),
        )
        conn.execute(
            "INSERT INTO draft_activity_log "
            "(league_id, manager_email, event_type, message, pick_number, player_id) "
            "VALUES (%s, %s, 'atomic_probe', 'must roll back', 1, %s)",
            (league_id, manager, player_id),
        )
        conn.execute(
            "INSERT INTO draft_picks "
            "(id, league_id, manager_email, pick_number, player_id, player_snapshot) "
            "VALUES (%s, %s, %s, 1, %s, '{}'::jsonb)",
            (pick_id, league_id, manager, player_id),
        )

    expect_atomic_rejection(connection, "draft pick", incomplete_pick)
    require(
        scalar(connection, "SELECT current_pick FROM draft_states WHERE league_id = %s", (league_id,)) == 1,
        "failed draft transaction advanced the pick",
    )
    require(
        scalar(connection, "SELECT COUNT(*) FROM draft_picks WHERE id = %s", (pick_id,)) == 0,
        "failed draft transaction retained the pick",
    )
    require(
        scalar(
            connection,
            "SELECT COUNT(*) FROM draft_activity_log WHERE league_id = %s AND event_type = 'atomic_probe'",
            (league_id,),
        )
        == 0,
        "failed draft transaction retained activity history",
    )

    with connection.transaction():
        connection.execute(
            "INSERT INTO rosters "
            "(league_id, manager_email, player_id, player_snapshot, roster_slot, acquired_via) "
            "VALUES (%s, %s, %s, '{}'::jsonb, 'bench', 'draft')",
            (league_id, manager, player_id),
        )
        connection.execute(
            "INSERT INTO draft_picks "
            "(id, league_id, manager_email, pick_number, player_id, player_snapshot) "
            "VALUES (%s, %s, %s, 1, %s, '{}'::jsonb)",
            (pick_id, league_id, manager, player_id),
        )
        connection.execute(
            "UPDATE draft_states SET current_pick = 2, version = 1 WHERE league_id = %s",
            (league_id,),
        )
        connection.execute("SET CONSTRAINTS ALL IMMEDIATE")

    require(roster_owner(connection, league_id, player_id) == manager, "valid draft roster assignment missing")
    require(scalar(connection, "SELECT COUNT(*) FROM draft_picks WHERE id = %s", (pick_id,)) == 1,
            "valid draft pick did not commit")


def waiver_contract(connection: Connection) -> None:
    league_id = f"atomic-waiver-{RUN_KEY}"
    manager = f"waiver-{RUN_KEY}@example.test"
    claim_id = f"claim-{RUN_KEY}"
    add_player = f"waiver-add-{RUN_KEY}"
    drop_player = f"waiver-drop-{RUN_KEY}"

    with connection.transaction():
        create_league(connection, league_id, [manager])
        connection.execute(
            "INSERT INTO waiver_states (league_id, version) VALUES (%s, 0)",
            (league_id,),
        )
        connection.execute(
            "INSERT INTO waiver_priorities (league_id, manager_email, priority) VALUES (%s, %s, 1)",
            (league_id, manager),
        )
        connection.execute(
            "INSERT INTO rosters "
            "(league_id, manager_email, player_id, player_snapshot, roster_slot, acquired_via) "
            "VALUES (%s, %s, %s, '{}'::jsonb, 'bench', 'draft')",
            (league_id, manager, drop_player),
        )
        connection.execute(
            "INSERT INTO waiver_claims "
            "(id, league_id, manager_email, add_player_id, add_player_snapshot, drop_player_id, status) "
            "VALUES (%s, %s, %s, %s, '{}'::jsonb, %s, 'pending')",
            (claim_id, league_id, manager, add_player, drop_player),
        )

    def incomplete_waiver(conn: Connection) -> None:
        conn.execute(
            "UPDATE waiver_states SET version = 1 WHERE league_id = %s",
            (league_id,),
        )
        conn.execute(
            "UPDATE waiver_priorities SET priority = 2 WHERE league_id = %s AND lower(manager_email) = lower(%s)",
            (league_id, manager),
        )
        conn.execute(
            "UPDATE waiver_claims SET status = 'processed', processed_at = NOW(), updated_at = NOW() "
            "WHERE id = %s",
            (claim_id,),
        )

    expect_atomic_rejection(connection, "waiver processing", incomplete_waiver)
    require(scalar(connection, "SELECT version FROM waiver_states WHERE league_id = %s", (league_id,)) == 0,
            "failed waiver transaction advanced its version")
    require(scalar(connection, "SELECT status FROM waiver_claims WHERE id = %s", (claim_id,)) == "pending",
            "failed waiver transaction changed claim status")
    require(roster_owner(connection, league_id, drop_player) == manager,
            "failed waiver transaction removed the drop player")
    require(roster_owner(connection, league_id, add_player) == "",
            "failed waiver transaction added the claimed player")

    with connection.transaction():
        connection.execute(
            "DELETE FROM rosters WHERE league_id = %s AND lower(manager_email) = lower(%s) AND player_id = %s",
            (league_id, manager, drop_player),
        )
        connection.execute(
            "INSERT INTO rosters "
            "(league_id, manager_email, player_id, player_snapshot, roster_slot, acquired_via) "
            "VALUES (%s, %s, %s, '{}'::jsonb, 'bench', 'waiver')",
            (league_id, manager, add_player),
        )
        connection.execute("UPDATE waiver_states SET version = 1 WHERE league_id = %s", (league_id,))
        connection.execute(
            "UPDATE waiver_claims SET status = 'processed', processed_at = NOW(), updated_at = NOW() "
            "WHERE id = %s",
            (claim_id,),
        )
        connection.execute("SET CONSTRAINTS ALL IMMEDIATE")

    require(roster_owner(connection, league_id, add_player) == manager, "valid waiver add did not commit")
    require(roster_owner(connection, league_id, drop_player) == "", "valid waiver drop did not commit")
    require(scalar(connection, "SELECT status FROM waiver_claims WHERE id = %s", (claim_id,)) == "processed",
            "valid waiver status did not commit")


def trade_contract(connection: Connection) -> None:
    league_id = f"atomic-trade-{RUN_KEY}"
    manager_a = f"trade-a-{RUN_KEY}@example.test"
    manager_b = f"trade-b-{RUN_KEY}@example.test"
    offer_id = f"trade-{RUN_KEY}"
    offered = [f"offer-one-{RUN_KEY}", f"offer-two-{RUN_KEY}"]
    requested = [f"request-one-{RUN_KEY}"]

    with connection.transaction():
        create_league(connection, league_id, [manager_a, manager_b])
        connection.execute("INSERT INTO trade_states (league_id, version) VALUES (%s, 0)", (league_id,))
        for player_id in offered:
            connection.execute(
                "INSERT INTO rosters "
                "(league_id, manager_email, player_id, player_snapshot, roster_slot, acquired_via) "
                "VALUES (%s, %s, %s, '{}'::jsonb, 'bench', 'draft')",
                (league_id, manager_a, player_id),
            )
        for player_id in requested:
            connection.execute(
                "INSERT INTO rosters "
                "(league_id, manager_email, player_id, player_snapshot, roster_slot, acquired_via) "
                "VALUES (%s, %s, %s, '{}'::jsonb, 'bench', 'draft')",
                (league_id, manager_b, player_id),
            )
        connection.execute(
            "INSERT INTO trade_offers "
            "(id, league_id, offered_by_email, offered_to_email, offered_player_ids, requested_player_ids, "
            " offer_player_snapshot, request_player_snapshot, target_manager, status) "
            "VALUES (%s, %s, %s, %s, %s, %s, '[]'::jsonb, '[]'::jsonb, %s, 'pending')",
            (offer_id, league_id, manager_a, manager_b, offered, requested, manager_b),
        )
        for player_id in offered:
            connection.execute(
                "INSERT INTO trade_player_locks "
                "(league_id, player_id, offer_id, manager_email, lock_role) "
                "VALUES (%s, %s, %s, %s, 'offered')",
                (league_id, player_id, offer_id, manager_a),
            )
        for player_id in requested:
            connection.execute(
                "INSERT INTO trade_player_locks "
                "(league_id, player_id, offer_id, manager_email, lock_role) "
                "VALUES (%s, %s, %s, %s, 'requested')",
                (league_id, player_id, offer_id, manager_b),
            )

    def incomplete_trade(conn: Connection) -> None:
        conn.execute("UPDATE trade_states SET version = 1 WHERE league_id = %s", (league_id,))
        conn.execute(
            "UPDATE trade_offers SET status = 'approved', state_version = 1, accepted_at = NOW(), "
            "approved_at = NOW(), resolved_at = NOW(), updated_at = NOW() WHERE id = %s",
            (offer_id,),
        )

    expect_atomic_rejection(connection, "trade execution", incomplete_trade)
    require(scalar(connection, "SELECT version FROM trade_states WHERE league_id = %s", (league_id,)) == 0,
            "failed trade transaction advanced its version")
    require(scalar(connection, "SELECT status FROM trade_offers WHERE id = %s", (offer_id,)) == "pending",
            "failed trade transaction changed offer status")
    require(scalar(connection, "SELECT COUNT(*) FROM trade_player_locks WHERE offer_id = %s", (offer_id,)) == 3,
            "failed trade transaction released player locks")
    require(all(roster_owner(connection, league_id, player_id) == manager_a for player_id in offered),
            "failed trade transaction moved offered players")
    require(all(roster_owner(connection, league_id, player_id) == manager_b for player_id in requested),
            "failed trade transaction moved requested players")

    with connection.transaction():
        for player_id in offered:
            connection.execute(
                "UPDATE rosters SET manager_email = %s, acquired_via = 'trade', acquired_at = NOW() "
                "WHERE league_id = %s AND player_id = %s",
                (manager_b, league_id, player_id),
            )
        for player_id in requested:
            connection.execute(
                "UPDATE rosters SET manager_email = %s, acquired_via = 'trade', acquired_at = NOW() "
                "WHERE league_id = %s AND player_id = %s",
                (manager_a, league_id, player_id),
            )
        connection.execute("DELETE FROM trade_player_locks WHERE offer_id = %s", (offer_id,))
        connection.execute("UPDATE trade_states SET version = 1 WHERE league_id = %s", (league_id,))
        connection.execute(
            "UPDATE trade_offers SET status = 'approved', state_version = 1, accepted_at = NOW(), "
            "approved_at = NOW(), resolved_at = NOW(), updated_at = NOW() WHERE id = %s",
            (offer_id,),
        )
        connection.execute("SET CONSTRAINTS ALL IMMEDIATE")

    require(all(roster_owner(connection, league_id, player_id) == manager_b for player_id in offered),
            "valid trade did not move offered package")
    require(all(roster_owner(connection, league_id, player_id) == manager_a for player_id in requested),
            "valid trade did not move requested package")
    require(scalar(connection, "SELECT status FROM trade_offers WHERE id = %s", (offer_id,)) == "approved",
            "valid trade status did not commit")
    require(scalar(connection, "SELECT COUNT(*) FROM trade_player_locks WHERE offer_id = %s", (offer_id,)) == 0,
            "valid trade retained player locks")


def main() -> None:
    with psycopg.connect(DB_URL, autocommit=True) as connection:
        draft_contract(connection)
        waiver_contract(connection)
        trade_contract(connection)
        connection.execute("DELETE FROM leagues WHERE id LIKE %s", (f"atomic-%-{RUN_KEY}",))
    print("atomic mutation runtime contract passed")


if __name__ == "__main__":
    main()
