#pragma once

#include <map>
#include <string>
#include <vector>

namespace cff::league_dashboard {

struct LineupWarning {
    std::string slot;
    int missing{0};
};

struct DashboardSignals {
    bool commissioner{false};
    int activeManagers{0};
    int teamCount{0};
    std::string draftStatus{"not_started"};
    bool hasLineupWarnings{false};
    int actionRequiredTrades{0};
    int pendingWaivers{0};
    bool hasCurrentMatchup{false};
    bool hasRoster{false};
};

struct NextAction {
    std::string code;
    std::string label;
    std::string href;
    std::string detail;
};

std::vector<LineupWarning> missingStarterWarnings(
    const std::map<std::string, int> &requiredSlots,
    const std::map<std::string, int> &assignedSlots,
    bool draftComplete);

NextAction chooseNextAction(const DashboardSignals &signals);

} // namespace cff::league_dashboard
