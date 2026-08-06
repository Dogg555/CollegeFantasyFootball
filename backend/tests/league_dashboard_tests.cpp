#include "league_dashboard.h"

#include <cassert>
#include <map>
#include <string>

using cff::league_dashboard::DashboardSignals;
using cff::league_dashboard::chooseNextAction;
using cff::league_dashboard::missingStarterWarnings;

int main() {
    const std::map<std::string, int> rules{
        {"qb", 1}, {"rb", 2}, {"wr", 2}, {"te", 1}, {"flex", 1}, {"bench", 5}
    };
    const std::map<std::string, int> assigned{{"qb", 1}, {"rb", 1}, {"wr", 2}};

    assert(missingStarterWarnings(rules, assigned, false).empty());
    const auto warnings = missingStarterWarnings(rules, assigned, true);
    assert(warnings.size() == 3);
    assert(warnings[0].slot == "flex" && warnings[0].missing == 1);
    assert(warnings[1].slot == "rb" && warnings[1].missing == 1);
    assert(warnings[2].slot == "te" && warnings[2].missing == 1);

    DashboardSignals signals;
    signals.commissioner = true;
    signals.teamCount = 4;
    signals.activeManagers = 2;
    assert(chooseNextAction(signals).code == "invite_managers");

    signals.activeManagers = 4;
    assert(chooseNextAction(signals).code == "open_draft");

    signals.draftStatus = "complete";
    signals.hasLineupWarnings = true;
    assert(chooseNextAction(signals).code == "fix_lineup");

    signals.hasLineupWarnings = false;
    signals.actionRequiredTrades = 1;
    assert(chooseNextAction(signals).code == "review_trade");

    signals.actionRequiredTrades = 0;
    signals.pendingWaivers = 2;
    assert(chooseNextAction(signals).code == "review_waivers");

    signals.pendingWaivers = 0;
    signals.hasCurrentMatchup = true;
    assert(chooseNextAction(signals).code == "view_matchup");

    signals.hasCurrentMatchup = false;
    signals.hasRoster = false;
    assert(chooseNextAction(signals).code == "browse_players");

    signals.hasRoster = true;
    assert(chooseNextAction(signals).code == "view_team");
    return 0;
}
