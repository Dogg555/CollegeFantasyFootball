#include "league_roster.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {
void expect(bool condition, const std::string &message) {
    if (!condition) {
        std::cerr << "lineup_assignment_tests failed: " << message << std::endl;
        std::exit(1);
    }
}

bool hasCode(const Json::Value &errors, const std::string &code) {
    for (const auto &error : errors) {
        if (error.isObject() && error.get("code", "").asString() == code) return true;
    }
    return false;
}

Json::Value rules() {
    Json::Value value(Json::objectValue);
    value["qb"] = 1; value["rb"] = 1; value["wr"] = 1; value["te"] = 0;
    value["flex"] = 1; value["k"] = 0; value["def"] = 0; value["bench"] = 2;
    return value;
}

Json::Value rosterPlayer(const std::string &id, const std::string &position, const std::string &slot) {
    Json::Value value(Json::objectValue);
    value["id"] = id; value["name"] = id; value["position"] = position; value["rosterSlot"] = slot;
    return value;
}

Json::Value assignment(const std::string &id, const std::string &slot) {
    Json::Value value(Json::objectValue);
    value["playerId"] = id; value["slot"] = slot;
    return value;
}
}

int main() {
    Json::Value roster(Json::arrayValue);
    roster.append(rosterPlayer("qb", "QB", "qb"));
    roster.append(rosterPlayer("rb", "RB", "rb"));
    roster.append(rosterPlayer("wr", "WR", "wr"));
    roster.append(rosterPlayer("flex", "TE", "flex"));
    roster.append(rosterPlayer("bench-qb", "QB", "bench"));
    roster.append(rosterPlayer("bench-rb", "RB", "bench"));

    Json::Value valid(Json::arrayValue);
    valid.append(assignment("qb", "bench"));
    valid.append(assignment("rb", "rb"));
    valid.append(assignment("wr", "wr"));
    valid.append(assignment("flex", "flex"));
    valid.append(assignment("bench-qb", "qb"));
    valid.append(assignment("bench-rb", "bench"));
    expect(cff::league_roster::lineupAssignmentErrors(roster, rules(), valid).empty(),
           "a complete eligible starter/bench swap must pass");

    auto duplicate = valid;
    duplicate[5]["playerId"] = "qb";
    expect(!cff::league_roster::lineupAssignmentErrors(roster, rules(), duplicate).empty(),
           "duplicate player assignments must fail");

    auto missing = valid;
    missing.resize(5);
    expect(!cff::league_roster::lineupAssignmentErrors(roster, rules(), missing).empty(),
           "partial assignment lists must fail");

    auto unknown = valid;
    unknown[5]["playerId"] = "outsider";
    expect(!cff::league_roster::lineupAssignmentErrors(roster, rules(), unknown).empty(),
           "unowned players must fail");

    auto ineligible = valid;
    ineligible[0]["slot"] = "wr";
    expect(!cff::league_roster::lineupAssignmentErrors(roster, rules(), ineligible).empty(),
           "ineligible slot assignments must fail");

    auto benchOverflow = valid;
    benchOverflow[4]["slot"] = "bench";
    expect(!cff::league_roster::lineupAssignmentErrors(roster, rules(), benchOverflow).empty(),
           "final bench assignments must respect configured capacity");

    auto scalarAssignment = valid;
    scalarAssignment[0] = "qb:bench";
    const auto scalarErrors = cff::league_roster::lineupAssignmentErrors(roster, rules(), scalarAssignment);
    expect(hasCode(scalarErrors, "invalid_lineup_assignment"),
           "scalar assignment entries must return a validation error without throwing");

    auto objectId = valid;
    objectId[0]["playerId"] = Json::Value(Json::objectValue);
    const auto idErrors = cff::league_roster::lineupAssignmentErrors(roster, rules(), objectId);
    expect(hasCode(idErrors, "lineup_player_required"),
           "non-string player IDs must return a validation error without throwing");

    auto objectSlot = valid;
    objectSlot[0]["slot"] = Json::Value(Json::objectValue);
    const auto slotErrors = cff::league_roster::lineupAssignmentErrors(roster, rules(), objectSlot);
    expect(hasCode(slotErrors, "invalid_lineup_slot"),
           "non-string slots must return a validation error without throwing");

    std::cout << "whole-lineup assignment contracts passed" << std::endl;
    return 0;
}
