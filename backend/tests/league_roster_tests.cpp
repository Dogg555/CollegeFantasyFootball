#include "league_roster.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <unordered_map>

namespace {

void expect(bool condition, const std::string &message) {
    if (!condition) {
        std::cerr << "league_roster_tests failed: " << message << std::endl;
        std::exit(1);
    }
}

Json::Value standardRules() {
    Json::Value rules(Json::objectValue);
    rules["qb"] = 1;
    rules["rb"] = 2;
    rules["wr"] = 2;
    rules["te"] = 1;
    rules["flex"] = 2;
    rules["bench"] = 6;
    return rules;
}

Json::Value player(const std::string &id, const std::string &position) {
    Json::Value value;
    value["id"] = id;
    value["position"] = position;
    return value;
}

Json::Value rosterPlayer(const std::string &id, const std::string &slot) {
    auto value = player(id, "WR");
    value["rosterSlot"] = slot;
    return value;
}

void testEligibilityContracts() {
    expect(cff::league_roster::flexEligible("RB"), "RB must remain FLEX eligible");
    expect(cff::league_roster::flexEligible("wr"), "WR must remain FLEX eligible");
    expect(cff::league_roster::flexEligible("Te"), "TE must remain FLEX eligible");
    expect(!cff::league_roster::flexEligible("QB"), "QB must not become FLEX eligible");
    expect(!cff::league_roster::flexEligible("K"), "K must not become FLEX eligible");

    const auto receiver = player("wr-1", "WR");
    const auto quarterback = player("qb-1", "QB");
    const auto kicker = player("k-1", "K");
    const auto defense = player("def-1", "DEF");
    expect(cff::league_roster::playerEligibleForSlot(receiver, "wr"),
           "natural position matching changed");
    expect(cff::league_roster::playerEligibleForSlot(receiver, "flex"),
           "receiver FLEX eligibility changed");
    expect(!cff::league_roster::playerEligibleForSlot(quarterback, "flex"),
           "quarterback must not enter FLEX");
    expect(cff::league_roster::playerEligibleForSlot(kicker, "k"),
           "kicker must be eligible for a configured K slot");
    expect(cff::league_roster::playerEligibleForSlot(defense, "def"),
           "team defense must be eligible for a configured DEF slot");
    expect(!cff::league_roster::playerEligibleForSlot(kicker, "flex"),
           "kicker must not enter FLEX");
    expect(!cff::league_roster::playerEligibleForSlot(defense, "flex"),
           "team defense must not enter FLEX");
    expect(cff::league_roster::playerEligibleForSlot(quarterback, "bench"),
           "bench must accept every player");
    expect(!cff::league_roster::playerEligibleForSlot(receiver, "WR"),
           "slot input normalization would change the existing API contract");
}

void testRosterSlotMoveContracts() {
    auto rules = standardRules();
    rules["k"] = 1;
    rules["def"] = 1;
    const auto receiver = player("wr-1", "WR");
    const auto kicker = player("k-1", "K");
    const auto defense = player("def-1", "DEF");
    Json::Value roster(Json::arrayValue);
    roster.append(rosterPlayer("wr-1", "wr"));
    roster.append(rosterPlayer("wr-2", "WR"));

    expect(cff::league_roster::validateRosterSlotMove(
               receiver, roster, rules, "wr-1", "wr"),
           "the moving player must not count against its destination capacity");
    expect(cff::league_roster::validateRosterSlotMove(
               kicker, roster, rules, "k-1", "k"),
           "configured kicker slot must accept a kicker");
    expect(cff::league_roster::validateRosterSlotMove(
               defense, roster, rules, "def-1", "def"),
           "configured defense slot must accept a team defense");

    roster.append(rosterPlayer("wr-3", "wr"));
    expect(!cff::league_roster::validateRosterSlotMove(
               receiver, roster, rules, "wr-1", "wr"),
           "a full natural slot must reject another player");
    expect(!cff::league_roster::validateRosterSlotMove(
               receiver, roster, rules, "wr-1", "rb"),
           "a player must not move to an ineligible natural slot");
    expect(!cff::league_roster::validateRosterSlotMove(
               receiver, roster, rules, "wr-1", "WR"),
           "uppercase slot names must retain their existing rejection behavior");
    expect(!cff::league_roster::validateRosterSlotMove(
               receiver, roster, rules, "wr-1", "superflex"),
           "unknown roster slots must remain rejected");
}

void testLineupValidationContracts() {
    Json::Value rules(Json::objectValue);
    rules["qb"] = 1;
    rules["rb"] = 2;
    rules["wr"] = 2;
    rules["te"] = 1;
    rules["flex"] = 1;
    rules["k"] = 1;
    rules["def"] = 1;

    const std::unordered_map<std::string, int> counts{
        {"qb", 1}, {"rb", 1}, {"wr", 3}, {"te", 1}, {"flex", 0}, {"k", 1}, {"def", 1}
    };
    const auto errors = cff::league_roster::lineupErrorsFromCounts(
        "manager@example.com", rules, counts);

    expect(errors.size() == 1, "only overfilled starter slots should be invalid");
    expect(errors[0]["managerEmail"].asString() == "manager@example.com",
           "lineup error manager identity changed");
    expect(errors[0]["slot"].asString() == "wr", "excess WR error order changed");
    expect(errors[0]["message"].asString() == "Too many WR starter(s)",
           "excess starter message changed");

    const auto emptyErrors = cff::league_roster::lineupErrorsFromCounts(
        "manager@example.com", rules, {});
    expect(emptyErrors.empty(),
           "empty starter slots must remain valid and score zero instead of blocking scoring");
}

void testRosterLimitContracts() {
    expect(cff::league_roster::rosterLimitFromRules(Json::Value{Json::arrayValue}) == 14,
           "non-object roster rules must retain the fourteen-player fallback");
    expect(cff::league_roster::rosterLimitFromRules(Json::Value{Json::objectValue}) == 14,
           "default roster rules must total fourteen players");

    Json::Value custom(Json::objectValue);
    custom["qb"] = 2;
    custom["rb"] = 3;
    custom["wr"] = 4;
    custom["te"] = 1;
    custom["flex"] = 1;
    custom["k"] = 1;
    custom["def"] = 1;
    custom["bench"] = 5;
    expect(cff::league_roster::rosterLimitFromRules(custom) == 18,
           "custom roster limit calculation must include configured K and DEF slots");
}

void testPreferredSlotContracts() {
    auto rules = standardRules();
    rules["k"] = 1;
    rules["def"] = 1;
    const auto receiver = player("wr-1", "WR");
    const auto quarterback = player("qb-1", "QB");
    const auto kicker = player("k-1", "K");
    const auto defense = player("def-1", "DEF");

    auto slot = cff::league_roster::preferredRosterSlot(
        receiver, rules, {{"wr", 1}, {"flex", 0}, {"bench", 0}});
    expect(slot && *slot == "wr", "open natural position must be preferred");

    slot = cff::league_roster::preferredRosterSlot(
        receiver, rules, {{"wr", 2}, {"flex", 1}, {"bench", 0}});
    expect(slot && *slot == "flex", "FLEX must be the second receiver destination");

    slot = cff::league_roster::preferredRosterSlot(
        receiver, rules, {{"wr", 2}, {"flex", 2}, {"bench", 5}});
    expect(slot && *slot == "bench", "bench must be the final available destination");

    slot = cff::league_roster::preferredRosterSlot(
        receiver, rules, {{"wr", 2}, {"flex", 2}, {"bench", 6}});
    expect(!slot, "a full roster must return no destination");

    slot = cff::league_roster::preferredRosterSlot(
        quarterback, rules, {{"qb", 1}, {"flex", 0}, {"bench", 0}});
    expect(slot && *slot == "bench", "QB must skip FLEX when its natural slot is full");

    slot = cff::league_roster::preferredRosterSlot(
        kicker, rules, {{"k", 0}, {"bench", 0}});
    expect(slot && *slot == "k", "kicker must prefer an open configured K slot");

    slot = cff::league_roster::preferredRosterSlot(
        defense, rules, {{"def", 0}, {"bench", 0}});
    expect(slot && *slot == "def", "team defense must prefer an open configured DEF slot");

    slot = cff::league_roster::preferredRosterSlot(
        receiver, rules, {{"wr", 1}, {"flex", 0}, {"bench", 0}}, 1);
    expect(slot && *slot == "flex", "transaction offsets must still count toward capacity");
}

} // namespace

int main() {
    testEligibilityContracts();
    testRosterSlotMoveContracts();
    testLineupValidationContracts();
    testRosterLimitContracts();
    testPreferredSlotContracts();
    std::cout << "league roster policy contracts passed" << std::endl;
    return 0;
}
