#include "league_trade.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void expect(bool condition, const std::string &message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

Json::Value player(const std::string &id, const std::string &name) {
    Json::Value value(Json::objectValue);
    value["id"] = id;
    value["name"] = name;
    return value;
}

Json::Value offer(const std::string &status,
                  const std::string &offeredBy,
                  const Json::Value &offeredPlayer,
                  const std::string &offeredTo,
                  const Json::Value &requestedPlayer) {
    Json::Value value(Json::objectValue);
    value["status"] = status;
    value["offeredByEmail"] = offeredBy;
    value["offerPlayer"] = offeredPlayer;
    value["offeredToEmail"] = offeredTo;
    value["requestPlayer"] = requestedPlayer;
    return value;
}

void testApprovalAndExpirationRules() {
    Json::Value rules(Json::objectValue);
    expect(!cff::league_trade::approvalRequired(rules),
           "commissioner approval should default to false");
    expect(cff::league_trade::expirationHours(rules) == 48,
           "trade expiration should default to 48 hours");

    rules["commissionerApproval"] = true;
    expect(cff::league_trade::approvalRequired(rules),
           "commissioner approval flag should be honored");

    rules["expirationHours"] = 0;
    expect(cff::league_trade::expirationHours(rules) == 1,
           "expiration should clamp to one hour");
    rules["expirationHours"] = 336;
    expect(cff::league_trade::expirationHours(rules) == 336,
           "expiration should allow the maximum");
    rules["expirationHours"] = 337;
    expect(cff::league_trade::expirationHours(rules) == 336,
           "expiration should clamp to 336 hours");
}

void testTargetAndStatusInputs() {
    expect(!cff::league_trade::validTarget("owner@example.com", ""),
           "empty trade target should be rejected");
    expect(!cff::league_trade::validTarget("owner@example.com", "owner@example.com"),
           "self trade target should be rejected");
    expect(cff::league_trade::validTarget("owner@example.com", "other@example.com"),
           "another manager should be a valid target");
    expect(cff::league_trade::validTarget("Owner@example.com", "owner@example.com"),
           "target comparison should preserve exact-case compatibility");

    for (const auto *status : {"Accepted", "Approved", "Vetoed", "Declined", "Cancelled"}) {
        expect(cff::league_trade::requestStatusAllowed(status),
               std::string{"allowed request status rejected: "} + status);
    }
    expect(!cff::league_trade::requestStatusAllowed("Pending"),
           "Pending should not be accepted as a status update");
    expect(!cff::league_trade::requestStatusAllowed("accepted"),
           "request status matching should remain exact and case-sensitive");

    expect(cff::league_trade::potentiallyExecutes("Accepted"),
           "Accepted may execute a trade");
    expect(cff::league_trade::potentiallyExecutes("Approved"),
           "Approved may execute a trade");
    expect(!cff::league_trade::potentiallyExecutes("Vetoed"),
           "Vetoed should not execute a trade");

    expect(cff::league_trade::openStatus("Pending"),
           "Pending UI status should be open");
    expect(cff::league_trade::openStatus("accepted"),
           "accepted database status should be open");
    expect(!cff::league_trade::openStatus("Approved"),
           "Approved should be closed");
}

void testStatusDecisions() {
    auto decision = cff::league_trade::decideStatus(
        "Accepted", false, true, false, true);
    expect(decision.allowed && decision.execute,
           "accepted trade without approval should execute");
    expect(decision.databaseStatus == "approved" && decision.displayStatus == "Approved",
           "auto-executed acceptance should become Approved");

    decision = cff::league_trade::decideStatus(
        "Accepted", true, true, false, true);
    expect(decision.allowed && !decision.execute,
           "accepted trade requiring approval should remain pending execution");
    expect(decision.databaseStatus == "accepted" && decision.displayStatus == "Accepted",
           "approval-required acceptance should remain Accepted");

    decision = cff::league_trade::decideStatus(
        "Accepted", false, false, false, true);
    expect(!decision.allowed,
           "database participant rules should reject outsider acceptance");

    decision = cff::league_trade::decideStatus(
        "Accepted", false, false, false, false);
    expect(decision.allowed && decision.execute,
           "local compatibility path should preserve acceptance without participant enforcement");

    decision = cff::league_trade::decideStatus(
        "Approved", true, true, false, true);
    expect(!decision.allowed && decision.commissionerRequired,
           "non-commissioners should not approve trades");

    decision = cff::league_trade::decideStatus(
        "Approved", true, false, true, true);
    expect(decision.allowed && decision.execute,
           "commissioner approval should execute the trade");

    decision = cff::league_trade::decideStatus(
        "Vetoed", true, false, true, true);
    expect(decision.allowed && !decision.execute
               && decision.databaseStatus == "vetoed"
               && decision.displayStatus == "Vetoed",
           "commissioner veto should resolve without execution");

    decision = cff::league_trade::decideStatus(
        "Declined", false, false, false, true);
    expect(!decision.allowed,
           "database participant rules should reject outsider decline");

    decision = cff::league_trade::decideStatus(
        "Cancelled", false, false, true, true);
    expect(decision.allowed && !decision.execute,
           "commissioner should be allowed to cancel a trade");

    decision = cff::league_trade::decideStatus(
        "Unknown", false, true, true, true);
    expect(!decision.allowed,
           "unknown trade status should be rejected");
}

void testOpenOfferLocks() {
    Json::Value offers(Json::arrayValue);
    offers.append(offer("Pending",
                        "alpha@example.com",
                        player("p-1", "Alpha Player"),
                        "beta@example.com",
                        player("p-2", "Beta Player")));
    offers.append(offer("Accepted",
                        "gamma@example.com",
                        player("p-3", "Gamma Player"),
                        "delta@example.com",
                        player("p-4", "Delta Player")));
    offers.append(offer("Approved",
                        "alpha@example.com",
                        player("p-5", "Closed Player"),
                        "beta@example.com",
                        player("p-6", "Closed Return")));

    expect(cff::league_trade::playerLockedInOpenOffer(
               offers, "alpha@example.com", "p-1"),
           "offered player should be locked by a pending trade");
    expect(cff::league_trade::playerLockedInOpenOffer(
               offers, "beta@example.com", "p-2"),
           "requested player should be locked by a pending trade");
    expect(cff::league_trade::playerLockedInOpenOffer(
               offers, "delta@example.com", "p-4"),
           "requested player should remain locked while Accepted");
    expect(!cff::league_trade::playerLockedInOpenOffer(
               offers, "alpha@example.com", "p-5"),
           "approved trades should not keep players locked");
    expect(!cff::league_trade::playerLockedInOpenOffer(
               offers, "other@example.com", "p-1"),
           "unrelated manager should not inherit another trade lock");
    expect(!cff::league_trade::playerLockedInOpenOffer(
               offers, "alpha@example.com", ""),
           "empty player IDs should never be locked");
}

void testTransactionSummaries() {
    const auto offered = player("p-1", "Test Player");
    expect(cff::league_trade::offerTransactionSummary(offered) == "Offered Test Player",
           "trade-offer summary must remain exact");
    expect(cff::league_trade::statusTransactionSummary("Approved", offered)
               == "Approved: Test Player",
           "trade-status summary must remain exact");

    Json::Value unnamed(Json::objectValue);
    expect(cff::league_trade::offerTransactionSummary(unnamed) == "Offered ",
           "missing-name summary compatibility should remain unchanged");
}

} // namespace

int main() {
    testApprovalAndExpirationRules();
    testTargetAndStatusInputs();
    testStatusDecisions();
    testOpenOfferLocks();
    testTransactionSummaries();
    std::cout << "league trade policy tests passed\n";
    return 0;
}
