#include "draft_lifecycle.h"

#include <cassert>
#include <iostream>
#include <string>
#include <unordered_map>

namespace {

Json::Value managers(int count) {
    Json::Value values(Json::arrayValue);
    for (int index = count; index >= 1; --index) {
        values.append("manager" + std::to_string(index) + "@example.com");
    }
    return values;
}

Json::Value defaultRules() {
    Json::Value rules(Json::objectValue);
    rules["qb"] = 1;
    rules["rb"] = 2;
    rules["wr"] = 2;
    rules["te"] = 1;
    rules["flex"] = 2;
    rules["bench"] = 6;
    return rules;
}

} // namespace

int main() {
    using namespace cff::draft_lifecycle;

    assert(canonicalEmail(" Manager@Example.COM ") == "manager@example.com");

    const auto four = canonicalOrder(managers(4));
    assert(four.size() == 4);
    assert(four[0].asString() == "manager1@example.com");
    assert(managerForPick(four, 1, "snake") == "manager1@example.com");
    assert(managerForPick(four, 4, "snake") == "manager4@example.com");
    assert(managerForPick(four, 5, "snake") == "manager4@example.com");
    assert(managerForPick(four, 8, "snake") == "manager1@example.com");

    const auto six = canonicalOrder(managers(6));
    assert(six.size() == 6);
    assert(managerForPick(six, 6, "snake") == "manager6@example.com");
    assert(managerForPick(six, 7, "snake") == "manager6@example.com");
    assert(managerForPick(six, 12, "snake") == "manager1@example.com");

    auto invalidOrder = four;
    invalidOrder[3] = "manager1@example.com";
    assert(orderMatchesManagers(four, managers(4)));
    assert(!orderMatchesManagers(invalidOrder, managers(4)));

    const auto rules = defaultRules();
    assert(rosterSlotsPerManager(rules) == 14);
    assert(totalDraftPicks(4, rules) == 56);
    assert(totalDraftPicks(6, rules) == 84);
    assert(!draftCompleteAfterPick(55, 4, rules));
    assert(draftCompleteAfterPick(56, 4, rules));
    assert(!draftCompleteAfterPick(83, 6, rules));
    assert(draftCompleteAfterPick(84, 6, rules));

    for (const auto *scenario : {&four, &six}) {
        const int managerCount = static_cast<int>(scenario->size());
        const int finalPick = totalDraftPicks(managerCount, rules);
        std::unordered_map<std::string, int> picksByManager;
        for (int pick = 1; pick <= finalPick; ++pick) {
            const auto manager = managerForPick(*scenario, pick, "snake");
            assert(!manager.empty());
            ++picksByManager[manager];
        }
        assert(static_cast<int>(picksByManager.size()) == managerCount);
        for (const auto &entry : picksByManager) {
            assert(entry.second == rosterSlotsPerManager(rules));
        }
    }

    Json::Value readiness(Json::arrayValue);
    for (const auto &email : four) {
        Json::Value entry(Json::objectValue);
        entry["email"] = email.asString();
        entry["ready"] = true;
        readiness.append(entry);
    }
    assert(allManagersReady(four, readiness));
    readiness[2]["ready"] = false;
    assert(!allManagersReady(four, readiness));

    std::cout << "draft lifecycle tests passed\n";
    return 0;
}
