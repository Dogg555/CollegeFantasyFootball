#include "trade_lifecycle.h"

#include <cassert>
#include <iostream>

using cff::trade_lifecycle::decideTransition;

int main() {
    const std::string offerer = "offerer@example.test";
    const std::string recipient = "recipient@example.test";
    const std::string commissioner = "commissioner@example.test";

    auto directAccept = decideTransition(
        "pending", "Accepted", recipient, offerer, recipient, false, false);
    assert(directAccept.allowed);
    assert(directAccept.execute);
    assert(directAccept.releaseLocks);
    assert(directAccept.nextStatus == "approved");

    auto approvalAccept = decideTransition(
        "pending", "Accepted", recipient, offerer, recipient, false, true);
    assert(approvalAccept.allowed);
    assert(!approvalAccept.execute);
    assert(!approvalAccept.releaseLocks);
    assert(approvalAccept.nextStatus == "accepted");

    auto approve = decideTransition(
        "accepted", "Approved", commissioner, offerer, recipient, true, true);
    assert(approve.allowed);
    assert(approve.execute);
    assert(approve.releaseLocks);
    assert(approve.nextStatus == "approved");

    auto earlyApprove = decideTransition(
        "pending", "Approved", commissioner, offerer, recipient, true, true);
    assert(!earlyApprove.allowed);
    assert(earlyApprove.errorCode == "trade_not_awaiting_approval");

    auto cancel = decideTransition(
        "accepted", "Cancelled", offerer, offerer, recipient, false, true);
    assert(cancel.allowed);
    assert(!cancel.execute);
    assert(cancel.releaseLocks);
    assert(cancel.nextStatus == "cancelled");

    auto invalidCancel = decideTransition(
        "pending", "Cancelled", recipient, offerer, recipient, false, false);
    assert(!invalidCancel.allowed);
    assert(invalidCancel.errorCode == "trade_offerer_required");

    auto decline = decideTransition(
        "pending", "Declined", recipient, offerer, recipient, false, false);
    assert(decline.allowed);
    assert(decline.releaseLocks);
    assert(decline.nextStatus == "declined");

    auto veto = decideTransition(
        "accepted", "Vetoed", commissioner, offerer, recipient, true, true);
    assert(veto.allowed);
    assert(!veto.execute);
    assert(veto.releaseLocks);
    assert(veto.nextStatus == "vetoed");

    auto closed = decideTransition(
        "approved", "Cancelled", offerer, offerer, recipient, false, false);
    assert(!closed.allowed);
    assert(closed.errorCode == "trade_closed");

    Json::Value body(Json::objectValue);
    body["expectedVersion"] = Json::Int64(7);
    assert(cff::trade_lifecycle::expectedVersionMatches(7, body));
    assert(!cff::trade_lifecycle::expectedVersionMatches(8, body));
    assert(cff::trade_lifecycle::validOfferPlayers("player-a", "player-b"));
    assert(!cff::trade_lifecycle::validOfferPlayers("player-a", "player-a"));
    assert(cff::trade_lifecycle::openStatus("Accepted"));
    assert(cff::trade_lifecycle::terminalStatus("Expired"));

    std::cout << "trade lifecycle tests passed\n";
    return 0;
}
