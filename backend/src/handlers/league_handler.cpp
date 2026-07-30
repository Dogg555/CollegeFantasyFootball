--- a/backend/src/handlers/league_handler.cpp
+++ b/backend/src/handlers/league_handler.cpp
@@
 namespace {
+// Forward declarations so functions used earlier in the file are visible.
+std::string jsonString(const Json::Value &body, const std::string &key, const std::string &fallback = "");
+Json::Value &arrayForLeague(std::unordered_map<std::string, Json::Value> &store, const std::string &leagueId);
+
