#include "roster_transaction.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

void expect(bool condition, const std::string &message) {
    if (!condition) {
        std::cerr << "roster_transaction_tests failed: " << message << std::endl;
        std::exit(1);
    }
}

Json::Value rules() {
    Json::Value value(Json::objectValue);
    value["qb"] = 1;
    value["rb"] = 1;
    value["wr"] = 1;
    value["te"] = 1;
    value["flex"] = 1;
    value["bench"] = 1;
    return value;
}

Json::Value player(const std::string &id,
                   const std::string &position,
                   const std::string &slot = "") {
    Json::Value value(Json::objectValue);
    value["id"] = id;
    value["name"] = id;
    value["position"] = position;
    if (!slot.empty()) value["rosterSlot"] = slot;
    return value;
}

void testWaiverGate() {
    Json::Value freeAgency(Json::objectValue);
    freeAgency["mode"] = "free_agency";
    freeAgency["freeAgencyLocked"] = false;
    expect(cff::roster_transaction::directAcquisitionAllowed(freeAgency),
           "open free agency must allow direct adds");

    auto waivers = freeAgency;
    waivers["mode"] = "waivers";
    expect(!cff::roster_transaction::directAcquisitionAllowed(waivers),
           "waiver mode must reject direct adds");

    auto locked = freeAgency;
    locked["freeAgencyLocked"] = true;
    expect(!cff::roster_transaction::directAcquisitionAllowed(locked),
           "commissioner free-agency lock must reject direct adds");
}

void testDestinationAfterDrop() {
    Json::Value roster(Json::arrayValue);
    roster.append(player("qb-1", "QB", "qb"));
    roster.append(player("rb-1", "RB", "rb"));
    roster.append(player("wr-1", "WR", "wr"));
    roster.append(player("te-1", "TE", "te"));
    roster.append(player("flex-1", "WR", "flex"));
    roster.append(player("bench-1", "QB", "bench"));

    expect(!cff::roster_transaction::destinationSlot(
               player("rb-2", "RB"), roster, rules()),
           "a full roster must reject a direct add");

    const auto natural = cff::roster_transaction::destinationSlot(
        player("rb-2", "RB"), roster, rules(), "rb-1");
    expect(natural && *natural == "rb",
           "dropping an RB must free the natural RB slot atomically");

    const auto bench = cff::roster_transaction::destinationSlot(
        player("qb-2", "QB"), roster, rules(), "bench-1");
    expect(bench && *bench == "bench",
           "dropping a bench player must free the bench slot");
}

void testIdentityAndCounts() {
    Json::Value roster(Json::arrayValue);
    roster.append(player(" alpha ", "WR", "WR"));
    roster.append(player("beta", "RB", "flex"));
    expect(cff::roster_transaction::canonicalPlayerId(" alpha ") == "alpha",
           "player identifiers must trim surrounding whitespace");
    expect(cff::roster_transaction::rosterContainsPlayer(roster, "alpha"),
           "normalized roster lookup failed");
    const auto counts = cff::roster_transaction::slotCounts(roster, "beta");
    expect(counts.at("wr") == 1, "slot names must normalize to lowercase");
    expect(counts.find("flex") == counts.end(),
           "the atomic drop candidate must not count against capacity");
}

void testVersionPrecondition() {
    Json::Value body(Json::objectValue);
    expect(cff::roster_transaction::expectedVersionMatches(7, body, false),
           "legacy requests may omit a version");
    expect(!cff::roster_transaction::expectedVersionMatches(7, body, true),
           "transaction requests must require a version");
    body["expectedVersion"] = 7;
    expect(cff::roster_transaction::expectedVersionMatches(7, body, true),
           "matching revision was rejected");
    body["expectedVersion"] = 6;
    expect(!cff::roster_transaction::expectedVersionMatches(7, body, true),
           "stale revision was accepted");
}

} // namespace

int main() {
    testWaiverGate();
    testDestinationAfterDrop();
    testIdentityAndCounts();
    testVersionPrecondition();
    std::cout << "roster transaction tests passed" << std::endl;
    return 0;
}
