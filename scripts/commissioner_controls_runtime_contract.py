#!/usr/bin/env python3
"""Production API/PostgreSQL contracts for safe commissioner controls."""
from __future__ import annotations

from urllib.parse import quote

import psycopg

from trade_lifecycle_runtime_contract import (
    DB_URL,
    RUN_KEY,
    call,
    expect,
    require,
    signup,
    wait_for_api,
)


def dashboard(league_id: str, token: str, status: int = 200):
    return expect(
        call("GET", f"/api/leagues/{league_id}/commissioner", token=token),
        status,
        "commissioner dashboard",
    )


def member_action(
    league_id: str,
    email: str,
    action: str,
    token: str,
    operation_key: str,
    status: int = 200,
):
    return expect(
        call(
            "POST",
            f"/api/leagues/{league_id}/commissioner/members/{quote(email, safe='')}/{action}",
            token=token,
            operation_key=operation_key,
            payload={"operationKey": operation_key},
        ),
        status,
        f"commissioner {action}",
    )


def main() -> None:
    wait_for_api()
    owner_email = f"commissioner-owner-{RUN_KEY}@example.test"
    manager_a = f"commissioner-a-{RUN_KEY}@example.test"
    manager_b = f"commissioner-b-{RUN_KEY}@example.test"
    manager_c = f"commissioner-c-{RUN_KEY}@example.test"
    emails = [owner_email, manager_a, manager_b, manager_c]
    tokens = {email: signup(email) for email in emails}

    created = expect(
        call(
            "POST",
            "/api/leagues",
            token=tokens[owner_email],
            operation_key=f"commissioner-create-{RUN_KEY}",
            payload={
                "name": f"Commissioner Contract {RUN_KEY}",
                "teams": 4,
                "scoring": "ppr",
                "draftType": "snake",
                "invitedEmails": [manager_a, manager_b],
                "rosterRules": {"qb": 1, "rb": 2, "wr": 2, "te": 1, "flex": 2, "bench": 6},
            },
        ),
        201,
        "create commissioner contract league",
    )
    league_id = str(created.get("id", ""))
    require(league_id, f"created league ID missing: {created!r}")

    initial = dashboard(league_id, tokens[owner_email])
    require(initial.get("actorIsOwner") is True, f"owner not recognized: {initial!r}")
    require(initial.get("actorIsCommissioner") is True, f"commissioner not recognized: {initial!r}")
    require(int(initial.get("teamCount", 0)) == 4, f"team count wrong: {initial!r}")
    require(initial.get("protectedSettingsLocked") is False, f"pre-draft settings unexpectedly locked: {initial!r}")
    dashboard(league_id, tokens[manager_a], 403)

    approve_a_key = f"approve-a-{RUN_KEY}"
    approved_a = member_action(league_id, manager_a, "approve", tokens[owner_email], approve_a_key)
    require(any(m.get("email") == manager_a and m.get("status") == "Active" for m in approved_a.get("members", [])),
            f"manager A was not approved: {approved_a!r}")
    replayed_a = member_action(league_id, manager_a, "approve", tokens[owner_email], approve_a_key)
    require(replayed_a.get("message") == approved_a.get("message"), "approval idempotency replay changed response")

    approved_b = member_action(
        league_id, manager_b, "approve", tokens[owner_email], f"approve-b-{RUN_KEY}"
    )
    require(int(approved_b.get("activeManagers", 0)) == 3, f"active manager count wrong: {approved_b!r}")

    promoted = member_action(
        league_id, manager_a, "promote", tokens[owner_email], f"promote-a-{RUN_KEY}"
    )
    require(any(m.get("email") == manager_a and m.get("role") == "commissioner" for m in promoted.get("members", [])),
            f"manager A was not promoted: {promoted!r}")
    promoted_dashboard = dashboard(league_id, tokens[manager_a])
    require(promoted_dashboard.get("actorIsOwner") is False, "promoted commissioner incorrectly became owner")

    invite_key = f"invite-c-{RUN_KEY}"
    invited_c = expect(
        call(
            "POST",
            f"/api/leagues/{league_id}/commissioner/invitations",
            token=tokens[manager_a],
            operation_key=invite_key,
            payload={"email": manager_c, "operationKey": invite_key},
        ),
        201,
        "promoted commissioner invite",
    )
    require(any(m.get("email") == manager_c and m.get("status") == "Invited" for m in invited_c.get("members", [])),
            f"manager C invitation missing: {invited_c!r}")

    member_action(
        league_id, manager_b, "transfer", tokens[manager_a], f"illegal-transfer-{RUN_KEY}", 403
    )

    transferred = member_action(
        league_id, manager_a, "transfer", tokens[owner_email], f"transfer-owner-{RUN_KEY}"
    )
    require(transferred.get("ownerEmail") == manager_a, f"ownership did not transfer: {transferred!r}")
    dashboard(league_id, tokens[owner_email], 403)
    new_owner_dashboard = dashboard(league_id, tokens[manager_a])
    require(new_owner_dashboard.get("actorIsOwner") is True, f"new owner not recognized: {new_owner_dashboard!r}")

    with psycopg.connect(DB_URL) as connection:
        with connection.cursor() as cursor:
            cursor.execute(
                "INSERT INTO draft_states (league_id, status, current_pick, draft_order) "
                "VALUES (%s, 'open', 1, ARRAY[%s, %s, %s]) "
                "ON CONFLICT (league_id) DO UPDATE SET status = 'open', updated_at = NOW()",
                (league_id, manager_a, owner_email, manager_b),
            )
        connection.commit()

    locked = dashboard(league_id, tokens[manager_a])
    require(locked.get("draftStarted") is True, f"draft start not detected: {locked!r}")
    require(locked.get("protectedSettingsLocked") is True, f"settings lock not reported: {locked!r}")

    settings_payload = {
        "name": created.get("name", f"Commissioner Contract {RUN_KEY}"),
        "teams": int(created.get("teams", 4)),
        "scoring": created.get("scoring", "ppr"),
        "draftType": created.get("draftType", "snake"),
        "draftDate": created.get("draftDate", ""),
        "draftLobbyOpen": bool(created.get("draftLobbyOpen", False)),
        "draftLobbyStartedAt": created.get("draftLobbyStartedAt", ""),
        "scoringSettings": created.get("scoringSettings", {}),
        "rosterRules": created.get("rosterRules", {}),
        "waiverRules": created.get("waiverRules", {}),
        "tradeRules": created.get("tradeRules", {}),
        "notes": f"Notes remain editable after draft {RUN_KEY}",
        "invitedEmails": created.get("invitedEmails", []),
    }
    saved_notes = expect(
        call(
            "PUT",
            f"/api/leagues/{league_id}/settings",
            token=tokens[manager_a],
            payload=settings_payload,
        ),
        200,
        "save unprotected settings after draft",
    )
    require(saved_notes.get("notes") == settings_payload["notes"], f"notes did not persist: {saved_notes!r}")

    with psycopg.connect(DB_URL) as connection:
        try:
            with connection.cursor() as cursor:
                cursor.execute("UPDATE leagues SET team_count = 6 WHERE id = %s", (league_id,))
            connection.commit()
            raise AssertionError("core team-count update unexpectedly bypassed competition lock")
        except psycopg.Error as error:
            connection.rollback()
            require(error.sqlstate == "55000", f"unexpected settings-lock SQLSTATE: {error.sqlstate} {error}")

    invite_after_start = expect(
        call(
            "POST",
            f"/api/leagues/{league_id}/commissioner/invitations",
            token=tokens[manager_a],
            operation_key=f"late-invite-{RUN_KEY}",
            payload={"email": f"late-{RUN_KEY}@example.test", "operationKey": f"late-invite-{RUN_KEY}"},
        ),
        409,
        "reject invite after draft start",
    )
    require(invite_after_start.get("code") == "draft_started", f"wrong late invite error: {invite_after_start!r}")

    remove_after_start = member_action(
        league_id, manager_b, "remove", tokens[manager_a], f"late-remove-{RUN_KEY}", 409
    )
    require(remove_after_start.get("code") == "draft_started", f"wrong late removal error: {remove_after_start!r}")

    with psycopg.connect(DB_URL) as connection:
        with connection.cursor() as cursor:
            cursor.execute(
                "SELECT COUNT(*) FROM commissioner_operations WHERE league_id = %s AND operation_key = %s",
                (league_id, approve_a_key),
            )
            require(cursor.fetchone()[0] == 1, "approval idempotency operation was not stored exactly once")

    print("commissioner controls runtime contract passed")


if __name__ == "__main__":
    main()
