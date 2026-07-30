--- a/backend/src/handlers/league_handler.cpp
+++ b/backend/src/handlers/league_handler.cpp
@@
 namespace {
+// Forward declarations so functions used earlier in the file are visible.
+std::string jsonString(const Json::Value &body, const std::string &key, const std::string &fallback = "");
+Json::Value &arrayForLeague(std::unordered_map<std::string, Json::Value> &store, const std::string &leagueId);
+
 bool waiverDeadlinePassed(const Json::Value &rules) {
     const auto deadline = cff::getStringOrDefault(rules, "claimDeadline", "");
     if (deadline.empty()) return true;
     return deadline <= isoNow();
 }
@@
     for (const auto &manager : managers) {
-        const auto email = cff::getStringOrDefault(manager, "email", "");
+        const auto email = cff::getStringOrDefault(manager, "email", "");
         if (!email.empty()) {
             emails.push_back(email);
         }
     }
@@
     for (const auto &member : membersIt->second) {
         if (cff::getStringOrDefault(member, "email", "") == accountEmail
             && cff::getStringOrDefault(member, "role", "") == "commissioner"
             && lowerString(cff::getStringOrDefault(member, "status", "Active")) != "removed") {
             return true;
         }
     }
@@
     for (const auto &member : membersIt->second) {
         if (cff::getStringOrDefault(member, "email", "") == accountEmail
             && lowerString(cff::getStringOrDefault(member, "status", "Active")) != "removed") {
             return true;
         }
     }
@@
     for (const auto &member : membersIt->second) {
         if (cff::getStringOrDefault(member, "email", "") == accountEmail
             && lowerString(cff::getStringOrDefault(member, "status", "Active")) == "active") {
             return true;
         }
     }
     return false;
 }
