#!/usr/bin/env python3
"""Phase 6 production contracts for multi-player trade packages and counteroffers."""
from __future__ import annotations

import json

import psycopg

from trade_lifecycle_runtime_contract import (
    DB_URL,
    RUN_KEY,
    call,
    configure_league,
    expect,
    lock_count,
    offer_by_id,
    owner,
    player,
    require,
    roster_version,
    signup,
    transaction,
    wait_for_api,
)


def create_league(
    label: str,
    emails: list[str],
    tokens: dict[str, str],
    roster_rules: dict[str, int],
) -> str:
    created = expect(
        call(
            "POST",
            "/api/leagues",
            token=tokens[emails[0]],
            operation_key=f"{label}-create-{RUN_KEY}",
            payload={
                "name": f"{label} {RUN_KEY}",
                "teams": 4,
                "scoring": "ppr",
                "draftType": "snake",
                "invitedEmails": emails[1:],
                "rosterRules": roster_rules,
            },
        ),
        201,
        f"create {label} league",
    )
    league_id = str(created.get("id", ""))
    require(league_id, f"created {label} league ID missing: {created!r}")
    return league_id


def require_code(payload: dict, code: str, label: str) -> None:
    require(str(payload.get("code", "")) == code, f"{label}: expected {code}, got {payload!r}")


def move_owner(league_id: str, player_id: str, manager_email: str) -> None:
    with psycopg.connect(DB_URL) as connection:
        with connection.cursor() as cursor:
            cursor.execute(
                "UPDATE rosters SET manager_email = %s WHERE league_id = %s AND player_id = %s",
                (manager_email, league_id, player_id),
            )
            require(cursor.rowcount == 1, f"could not move {player_id} for stale-ownership regression")
        connection.commit()


def roster_count(league_id: str, manager_email: str) -> int:
    with psycopg.connect(DB_URL) as connection:
        with connection.cursor() as cursor:
            cursor.execute(
                "SELECT COUNT(*) FROM rosters WHERE league_id = %s AND lower(manager_email) = lower(%s)",
                (league_id, manager_email),
            )
            return int(cursor.fetchone()[0])


def package_ids(prefix: str, count: int) -> list[str]:
    return [f"{prefix}-{index}-{RUN_KEY}" for index in range(1, count + 1)]


def assert_owner_package(league_id: str, ids: list[str], expected: str, label: str) -> None:
    for player_id in ids:
        require(owner(league_id, player_id) == expected, f"{label}: {player_id} not owned by {expected}")


def execute_package(
    league_id: str,
    tokens: dict[str, str],
    offerer: str,
    recipient: str,
    state: dict,
    offered: list[dict],
    requested: list[dict],
    label: str,
) -> dict:
    before_offerer = roster_version(league_id, offerer)
    before_recipient = roster_version(league_id, recipient)
    key = f"{label}-create-{RUN_KEY}"
    expected_version = int(state["version"])
    created = expect(
        transaction(
            league_id,
            tokens[offerer],
            key,
            "create",
            expected_version,
            offerPlayers=offered,
            requestPlayers=requested,
            targetManager=recipient,
            note=label,
        ),
        200,
        f"create {label}",
    )
    trade_id = str(created.get("tradeId", ""))
    require(trade_id, f"{label}: missing trade id")
    offer = offer_by_id(created, trade_id)
    require(len(offer.get("offerPlayers", [])) == len(offered), f"{label}: offered package collapsed")
    require(len(offer.get("requestPlayers", [])) == len(requested), f"{label}: requested package collapsed")
    require(lock_count(league_id, trade_id) == len(offered) + len(requested), f"{label}: not every player locked")

    # Same-key retry must replay the already-created package rather than duplicate it.
    replay = expect(
        transaction(
            league_id,
            tokens[offerer],
            key,
            "create",
            expected_version,
            offerPlayers=offered,
            requestPlayers=requested,
            targetManager=recipient,
            note=label,
        ),
        200,
        f"replay {label} create",
    )
    require(str(replay.get("tradeId", "")) == trade_id, f"{label}: create replay produced a second trade")
    require(int(replay.get("version", -1)) == int(created["version"]), f"{label}: create replay changed version")

    accepted = expect(
        transaction(
            league_id,
            tokens[recipient],
            f"{label}-accept-{RUN_KEY}",
            "status",
            int(created["version"]),
            tradeId=trade_id,
            status="Accepted",
        ),
        200,
        f"accept {label}",
    )
    require(offer_by_id(accepted, trade_id).get("status") == "Approved", f"{label}: trade did not execute")
    assert_owner_package(league_id, [str(item["id"]) for item in offered], recipient, label)
    assert_owner_package(league_id, [str(item["id"]) for item in requested], offerer, label)
    require(lock_count(league_id, trade_id) == 0, f"{label}: completed trade retained locks")
    require(roster_version(league_id, offerer) == before_offerer + 1, f"{label}: offerer roster version advanced incorrectly")
    require(roster_version(league_id, recipient) == before_recipient + 1, f"{label}: recipient roster version advanced incorrectly")
    return accepted


def main() -> None:
    wait_for_api()
    emails = [
        f"phase6-commissioner-{RUN_KEY}@example.test",
        f"phase6-manager-a-{RUN_KEY}@example.test",
        f"phase6-manager-b-{RUN_KEY}@example.test",
        f"phase6-member-{RUN_KEY}@example.test",
    ]
    tokens = {email: signup(email) for email in emails}
    commissioner, manager_a, manager_b, member = emails

    roster_rules = {"qb": 1, "rb": 2, "wr": 2, "te": 1, "flex": 2, "bench": 6}
    league_id = create_league("Phase 6 Multi Trade", emails, tokens, roster_rules)

    a_ids = package_ids("phase6-a", 10)
    b_ids = package_ids("phase6-b", 10)
    a_positions = ["WR", "WR", "RB", "RB", "QB", "TE", "WR", "RB", "WR", "RB"]
    b_positions = ["RB", "WR", "QB", "TE", "WR", "RB", "WR", "RB", "WR", "RB"]
    slots = ["wr", "wr", "rb", "rb", "qb", "te", "flex", "flex", "bench", "bench"]
    a_players = [player(pid, f"Phase6 A{index}", a_positions[index - 1]) for index, pid in enumerate(a_ids, start=1)]
    b_players = [player(pid, f"Phase6 B{index}", b_positions[index - 1]) for index, pid in enumerate(b_ids, start=1)]
    seeded: dict[str, tuple[str, dict, str]] = {}
    for index, snapshot in enumerate(a_players):
        seeded[str(snapshot["id"])] = (manager_a, snapshot, slots[index])
    for index, snapshot in enumerate(b_players):
        seeded[str(snapshot["id"])] = (manager_b, snapshot, slots[index])
    configure_league(league_id, emails, seeded)

    state = expect(
        call("GET", f"/api/leagues/{league_id}/trades/state", token=tokens[manager_a]),
        200,
        "initial Phase 6 trade state",
    )
    require(state.get("multiPlayerTrades") is True, f"multi-player capability missing: {state!r}")

    # Duplicate assets are rejected without advancing trade state or acquiring locks.
    duplicate = expect(
        transaction(
            league_id,
            tokens[manager_a],
            f"phase6-duplicate-{RUN_KEY}",
            "create",
            int(state["version"]),
            offerPlayers=[a_players[0], a_players[0]],
            requestPlayers=[b_players[0]],
            targetManager=manager_b,
            note="duplicate asset regression",
        ),
        409,
        "reject duplicate offered asset",
    )
    require(
        str(duplicate.get("code", "")) in {"invalid_trade_players", "offered_player_not_owned"},
        f"duplicate asset returned an unexpected error: {duplicate!r}",
    )
    duplicate_state = expect(
        call("GET", f"/api/leagues/{league_id}/trades/state", token=tokens[manager_a]),
        200,
        "state after duplicate rejection",
    )
    require(int(duplicate_state["version"]) == int(state["version"]), "duplicate rejection advanced trade version")
    require(not duplicate_state.get("offers"), f"duplicate rejection created an offer: {duplicate_state!r}")

    # Exact action-plan exit gate: legal asymmetric packages in all required directions.
    state = execute_package(
        league_id, tokens, manager_a, manager_b, state,
        [a_players[0]], [b_players[0], b_players[1]], "phase6-1-for-2",
    )
    state = execute_package(
        league_id, tokens, manager_a, manager_b, state,
        [a_players[1], a_players[2]], [b_players[2], b_players[3], b_players[4]], "phase6-2-for-3",
    )
    state = execute_package(
        league_id, tokens, manager_a, manager_b, state,
        [a_players[3], a_players[4], a_players[5]], [b_players[5]], "phase6-3-for-1",
    )

    # Stale expected-version proposals must not mutate either roster.
    stale_base_version = int(state["version"])
    stale_created = expect(
        transaction(
            league_id,
            tokens[manager_a],
            f"phase6-stale-create-{RUN_KEY}",
            "create",
            stale_base_version,
            offerPlayers=[a_players[8]],
            requestPlayers=[b_players[8]],
            targetManager=manager_b,
            note="stale version regression",
        ),
        200,
        "create stale-version trade",
    )
    stale_id = str(stale_created["tradeId"])
    stale = expect(
        transaction(
            league_id,
            tokens[manager_b],
            f"phase6-stale-accept-{RUN_KEY}",
            "status",
            stale_base_version,
            tradeId=stale_id,
            status="Accepted",
        ),
        409,
        "reject stale trade transition",
    )
    require_code(stale, "trade_state_conflict", "stale trade transition")
    require(owner(league_id, a_ids[8]) == manager_a, "stale transition moved offered player")
    require(owner(league_id, b_ids[8]) == manager_b, "stale transition moved requested player")

    # A third manager and the offerer cannot accept; the receiver can cleanly reject.
    outsider = expect(
        transaction(
            league_id,
            tokens[member],
            f"phase6-outsider-accept-{RUN_KEY}",
            "status",
            int(stale_created["version"]),
            tradeId=stale_id,
            status="Accepted",
        ),
        403,
        "reject outsider accept",
    )
    require_code(outsider, "trade_access_required", "outsider accept")
    self_accept = expect(
        transaction(
            league_id,
            tokens[manager_a],
            f"phase6-self-accept-{RUN_KEY}",
            "status",
            int(stale_created["version"]),
            tradeId=stale_id,
            status="Accepted",
        ),
        409,
        "reject offerer accept",
    )
    require_code(self_accept, "trade_recipient_required", "offerer accept")
    offerer_counter = expect(
        transaction(
            league_id,
            tokens[manager_a],
            f"phase6-self-counter-{RUN_KEY}",
            "counter",
            int(stale_created["version"]),
            tradeId=stale_id,
        ),
        409,
        "reject offerer counter",
    )
    require_code(offerer_counter, "trade_counter_not_allowed", "offerer counter")
    declined = expect(
        transaction(
            league_id,
            tokens[manager_b],
            f"phase6-decline-{RUN_KEY}",
            "status",
            int(stale_created["version"]),
            tradeId=stale_id,
            status="Declined",
        ),
        200,
        "receiver declines trade",
    )
    require(offer_by_id(declined, stale_id).get("status") == "Declined", "receiver decline did not persist")
    require(lock_count(league_id, stale_id) == 0, "declined trade retained locks")
    state = declined

    # Counter creates a new reversed package, closes the source, and remains permission-safe.
    source = expect(
        transaction(
            league_id,
            tokens[manager_a],
            f"phase6-counter-source-{RUN_KEY}",
            "create",
            int(state["version"]),
            offerPlayers=[a_players[6]],
            requestPlayers=[b_players[6]],
            targetManager=manager_b,
            note="counter source",
        ),
        200,
        "create counter source",
    )
    source_id = str(source["tradeId"])
    counter = expect(
        transaction(
            league_id,
            tokens[manager_b],
            f"phase6-counter-create-{RUN_KEY}",
            "counter",
            int(source["version"]),
            tradeId=source_id,
            offerPlayers=[b_players[6], b_players[7]],
            requestPlayers=[a_players[6], a_players[7]],
            targetManager=manager_a,
            note="two-for-two counter",
        ),
        200,
        "create counteroffer",
    )
    counter_id = str(counter["tradeId"])
    require(counter_id and counter_id != source_id, "counter did not create a distinct proposal")
    require(offer_by_id(counter, source_id).get("status") == "Countered", "counter source not marked Countered")
    require(lock_count(league_id, source_id) == 0, "counter source retained locks")
    require(lock_count(league_id, counter_id) == 4, "counter did not lock all package players")
    counter_accepted = expect(
        transaction(
            league_id,
            tokens[manager_a],
            f"phase6-counter-accept-{RUN_KEY}",
            "status",
            int(counter["version"]),
            tradeId=counter_id,
            status="Accepted",
        ),
        200,
        "accept counteroffer",
    )
    require(offer_by_id(counter_accepted, counter_id).get("status") == "Approved", "counteroffer did not execute")
    assert_owner_package(league_id, [b_ids[6], b_ids[7]], manager_a, "counter")
    assert_owner_package(league_id, [a_ids[6], a_ids[7]], manager_b, "counter")
    state = counter_accepted

    # Ownership changes between proposal and acceptance must fail before any package mutation.
    ownership_created = expect(
        transaction(
            league_id,
            tokens[manager_a],
            f"phase6-ownership-create-{RUN_KEY}",
            "create",
            int(state["version"]),
            offerPlayers=[a_players[9]],
            requestPlayers=[b_players[9]],
            targetManager=manager_b,
            note="ownership changes before accept",
        ),
        200,
        "create ownership-change trade",
    )
    ownership_id = str(ownership_created["tradeId"])
    move_owner(league_id, b_ids[9], member)
    changed = expect(
        transaction(
            league_id,
            tokens[manager_b],
            f"phase6-ownership-accept-{RUN_KEY}",
            "status",
            int(ownership_created["version"]),
            tradeId=ownership_id,
            status="Accepted",
        ),
        409,
        "reject changed ownership",
    )
    require_code(changed, "trade_ownership_changed", "changed ownership")
    require(owner(league_id, a_ids[9]) == manager_a, "ownership failure partially moved offered player")
    require(owner(league_id, b_ids[9]) == member, "ownership failure changed the new owner")

    # Impossible asymmetric packages must rollback with both rosters untouched.
    cap_league = create_league(
        "Phase 6 Capacity",
        emails,
        tokens,
        {"qb": 1, "rb": 0, "wr": 0, "te": 0, "flex": 0, "bench": 1},
    )
    cap_a = [player(f"phase6-cap-a-{index}-{RUN_KEY}", f"Capacity A{index}", "QB") for index in (1, 2)]
    cap_b = [player(f"phase6-cap-b-{index}-{RUN_KEY}", f"Capacity B{index}", "QB") for index in (1, 2)]
    configure_league(
        cap_league,
        emails,
        {
            cap_a[0]["id"]: (manager_a, cap_a[0], "qb"),
            cap_a[1]["id"]: (manager_a, cap_a[1], "bench"),
            cap_b[0]["id"]: (manager_b, cap_b[0], "qb"),
            cap_b[1]["id"]: (manager_b, cap_b[1], "bench"),
        },
    )
    cap_state = expect(
        call("GET", f"/api/leagues/{cap_league}/trades/state", token=tokens[manager_a]),
        200,
        "capacity trade state",
    )
    cap_created = expect(
        transaction(
            cap_league,
            tokens[manager_a],
            f"phase6-capacity-create-{RUN_KEY}",
            "create",
            int(cap_state["version"]),
            offerPlayers=[cap_a[0]],
            requestPlayers=cap_b,
            targetManager=manager_b,
            note="must exceed two-player roster capacity",
        ),
        200,
        "create invalid-capacity package",
    )
    before_a_count = roster_count(cap_league, manager_a)
    before_b_count = roster_count(cap_league, manager_b)
    before_a_version = roster_version(cap_league, manager_a)
    before_b_version = roster_version(cap_league, manager_b)
    cap_rejected = expect(
        transaction(
            cap_league,
            tokens[manager_b],
            f"phase6-capacity-accept-{RUN_KEY}",
            "status",
            int(cap_created["version"]),
            tradeId=str(cap_created["tradeId"]),
            status="Accepted",
        ),
        409,
        "reject over-capacity package",
    )
    require_code(cap_rejected, "trade_roster_invalid", "over-capacity package")
    require(roster_count(cap_league, manager_a) == before_a_count == 2, "capacity failure changed manager A roster size")
    require(roster_count(cap_league, manager_b) == before_b_count == 2, "capacity failure changed manager B roster size")
    assert_owner_package(cap_league, [str(item["id"]) for item in cap_a], manager_a, "capacity rollback")
    assert_owner_package(cap_league, [str(item["id"]) for item in cap_b], manager_b, "capacity rollback")
    require(roster_version(cap_league, manager_a) == before_a_version, "capacity failure advanced manager A roster version")
    require(roster_version(cap_league, manager_b) == before_b_version, "capacity failure advanced manager B roster version")

    report = {
        "status": "passed",
        "legalPackages": ["1-for-2", "2-for-3", "3-for-1"],
        "staleProposalRejected": True,
        "permissionChecks": ["outsider-accept", "offerer-accept", "offerer-counter", "receiver-decline"],
        "counter": "passed",
        "ownershipChangeRejected": True,
        "atomicCapacityRollback": True,
    }
    print(json.dumps(report, sort_keys=True))


if __name__ == "__main__":
    main()
