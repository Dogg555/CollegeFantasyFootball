#pragma once

#include <drogon/drogon.h>

#include <string>

namespace cff::schedule_lineup_hardening {

// Returns nullptr when scoring may continue. A non-null response is a stable
// client error or storage error that should be returned instead of scoring.
drogon::HttpResponsePtr prepareLineupsForScoring(const std::string &leagueId,
                                                 const std::string &accountEmail,
                                                 int season,
                                                 int week);

} // namespace cff::schedule_lineup_hardening
