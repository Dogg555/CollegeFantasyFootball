#!/usr/bin/env python3
"""Runtime contracts for multi-player trade packages and counteroffers."""
from __future__ import annotations

from trade_lifecycle_runtime_contract import (
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


def main() -> None:
    wait_for_api()
    emails = [
        f"multi-commissioner-{RUN_KEY}@example.test",
        f"multi-manager-a-{RUN_KEY}@example.test",
        f"multi-manager-b-{RUN_KEY}@example.test",
        f"multi-member-{RUN_KEY}@example.test",
    ]
    tokens = {email: signup(email) for email in emails}
    commissioner, manager_a, manager_b, _member = emails

    created = expect(
        call(
            "POST",
            "/api/leagues",
            token=tokens[commissioner],
            operation_key=f"multi-create-{RUN_KEY}",
            payload={
                "name": f"Multi Trade Contract {RUN_KEY}",
                "teams": 4,
                "scoring": "ppr",
                "draftType": "snake",
                "invitedEmails": emails[1:],
                "rosterRules": {"qb": 1, "rb": 2, "wr": 2, "te": 1, "flex": 2, "bench": 6},
            },
        ),
        201,
        "create multi-player trade league",
    )
    league_id = str(created.get("id", ""))
    require(league_id, f"created league ID missing: {created!r}")

    a = player(f"multi-a-{RUN_KEY}", "Package Alpha", "WR")
    b = player(f"multi-b-{RUN_KEY}", "Package Beta", "WR")
    c = player(f"multi-c-{RUN_KEY}", "Package Charlie", "RB")
    d = player(f"multi-d-{RUN_KEY}", "Counter Delta", "QB")
    e = player(f"multi-e-{RUN_KEY}", "Counter Echo", "TE")
    f = player(f"multi-f-{RUN_KEY}", "Counter Foxtrot", "RB")
    g = player(f"multi-g-{RUN_KEY}", "Counter Golf", "WR")
    players = {
        a["id"]: (manager_a, a, "wr"),
        b["id"]: (manager_a, b, "wr"),
        c["id"]: (manager_b, c, "rb"),
        d["id"]: (manager_a, d, "qb"),
        e["id"]: (manager_a, e, "te"),
        f["id"]: (manager_b, f, "rb"),
        g["id"]: (manager_b, g, "wr"),
    }
    configure_league(league_id, emails, players)

    initial = expect(
        call("GET", f"/api/leagues/{league_id}/trades/state", token=tokens[manager_a]),
        200,
        "initial multi-player state",
    )
    require(initial.get("multiPlayerTrades") is True, f"multi-player capability missing: {initial!r}")
    require(int(initial.get("version", -1)) == 0, f"initial trade version wrong: {initial!r}")

    manager_a_before = roster_version(league_id, manager_a)
    manager_b_before = roster_version(league_id, manager_b)
    package = expect(
        transaction(
            league_id,
            tokens[manager_a],
            f"multi-package-create-{RUN_KEY}",
            "create",
            0,
            offerPlayers=[a, b],
            requestPlayers=[c],
            targetManager=manager_b,
            note="Two wide receivers for one running back",
        ),
        200,
        "create 2-for-1 trade",
    )
    package_id = str(package.get("tradeId", ""))
    package_offer = offer_by_id(package, package_id)
    require(len(package_offer.get("offerPlayers", [])) == 2, f"offered package collapsed: {package_offer!r}")
    require(len(package_offer.get("requestPlayers", [])) == 1, f"requested package collapsed: {package_offer!r}")
    require(lock_count(league_id, package_id) == 3, "2-for-1 trade did not lock every player")

    accepted = expect(
        transaction(
            league_id,
            tokens[manager_b],
            f"multi-package-accept-{RUN_KEY}",
            "status",
            int(package["version"]),
            tradeId=package_id,
            status="Accepted",
        ),
        200,
        "accept 2-for-1 trade",
    )
    accepted_offer = offer_by_id(accepted, package_id)
    require(accepted_offer.get("status") == "Approved", f"2-for-1 trade did not execute: {accepted_offer!r}")
    require(owner(league_id, a["id"]) == manager_b, "first offered player did not move atomically")
    require(owner(league_id, b["id"]) == manager_b, "second offered player did not move atomically")
    require(owner(league_id, c["id"]) == manager_a, "requested player did not move atomically")
    require(lock_count(league_id, package_id) == 0, "completed package retained player locks")
    require(roster_version(league_id, manager_a) == manager_a_before + 1,
            "2-for-1 trade did not advance manager A roster version exactly once")
    require(roster_version(league_id, manager_b) == manager_b_before + 1,
            "2-for-1 trade did not advance manager B roster version exactly once")

    counter_source_state = expect(
        call("GET", f"/api/leagues/{league_id}/trades/state", token=tokens[manager_a]),
        200,
        "counter source state",
    )
    source = expect(
        transaction(
            league_id,
            tokens[manager_a],
            f"counter-source-{RUN_KEY}",
            "create",
            int(counter_source_state["version"]),
            offerPlayers=[d],
            requestPlayers=[f],
            targetManager=manager_b,
            note="Initial counterable offer",
        ),
        200,
        "create counterable trade",
    )
    source_id = str(source["tradeId"])
    require(lock_count(league_id, source_id) == 2, "counter source did not lock both players")

    counter = expect(
        transaction(
            league_id,
            tokens[manager_b],
            f"counter-create-{RUN_KEY}",
            "counter",
            int(source["version"]),
            tradeId=source_id,
            offerPlayers=[f, g],
            requestPlayers=[d, e],
            targetManager=manager_a,
            note="Two-for-two counteroffer",
        ),
        200,
        "create counteroffer",
    )
    counter_id = str(counter["tradeId"])
    require(counter_id and counter_id != source_id, f"counter did not create a new offer: {counter!r}")
    source_offer = offer_by_id(counter, source_id)
    counter_offer = offer_by_id(counter, counter_id)
    require(source_offer.get("status") == "Countered", f"original offer not marked countered: {source_offer!r}")
    require(counter_offer.get("offeredByEmail") == manager_b, f"counter offerer wrong: {counter_offer!r}")
    require(counter_offer.get("offeredToEmail") == manager_a, f"counter recipient wrong: {counter_offer!r}")
    require(len(counter_offer.get("offerPlayers", [])) == 2, f"counter offered package collapsed: {counter_offer!r}")
    require(len(counter_offer.get("requestPlayers", [])) == 2, f"counter requested package collapsed: {counter_offer!r}")
    require(lock_count(league_id, source_id) == 0, "countered offer retained old locks")
    require(lock_count(league_id, counter_id) == 4, "counteroffer did not lock all four players")

    counter_accepted = expect(
        transaction(
            league_id,
            tokens[manager_a],
            f"counter-accept-{RUN_KEY}",
            "status",
            int(counter["version"]),
            tradeId=counter_id,
            status="Accepted",
        ),
        200,
        "accept counteroffer",
    )
    final_counter = offer_by_id(counter_accepted, counter_id)
    require(final_counter.get("status") == "Approved", f"counteroffer did not execute: {final_counter!r}")
    require(owner(league_id, f["id"]) == manager_a, "first counter-offered player did not move")
    require(owner(league_id, g["id"]) == manager_a, "second counter-offered player did not move")
    require(owner(league_id, d["id"]) == manager_b, "first counter-requested player did not move")
    require(owner(league_id, e["id"]) == manager_b, "second counter-requested player did not move")
    require(lock_count(league_id, counter_id) == 0, "completed counteroffer retained locks")

    print("multi-player trade runtime contract passed")


if __name__ == "__main__":
    main()
