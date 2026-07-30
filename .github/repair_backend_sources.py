#!/usr/bin/env python3
from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected one match, found {count}")
    return text.replace(old, new, 1)


main_path = Path("backend/src/main.cpp")
main = main_path.read_text(encoding="utf-8")
main_replacements = {
    'payload["counts"]["teams"] = std::stoll(PQgetvalue(counts.get(), 0, 0));':
        'payload["counts"]["teams"] = static_cast<Json::Int64>(std::stoll(PQgetvalue(counts.get(), 0, 0)));',
    'payload["counts"]["players"] = std::stoll(PQgetvalue(counts.get(), 0, 1));':
        'payload["counts"]["players"] = static_cast<Json::Int64>(std::stoll(PQgetvalue(counts.get(), 0, 1)));',
    'payload["counts"]["games"] = std::stoll(PQgetvalue(counts.get(), 0, 2));':
        'payload["counts"]["games"] = static_cast<Json::Int64>(std::stoll(PQgetvalue(counts.get(), 0, 2)));',
    'payload["counts"]["playerStats"] = std::stoll(PQgetvalue(counts.get(), 0, 3));':
        'payload["counts"]["playerStats"] = static_cast<Json::Int64>(std::stoll(PQgetvalue(counts.get(), 0, 3)));',
    'run["id"] = std::stoll(PQgetvalue(runs.get(), row, 0));':
        'run["id"] = static_cast<Json::Int64>(std::stoll(PQgetvalue(runs.get(), row, 0)));',
}
for old, new in main_replacements.items():
    main = replace_once(main, old, new, f"main.cpp replacement: {old}")
main_path.write_text(main, encoding="utf-8")

league_path = Path("backend/src/handlers/league_handler.cpp")
league = league_path.read_text(encoding="utf-8")
anchor = "namespace {\nconstexpr std::size_t kMaxLeaguesPerAccount = 3;"
forward_declarations = """namespace {
// Helpers are defined later in this translation unit, but several early
// validation functions use them. Keep declarations here so the full source
// compiles without moving or duplicating implementation code.
std::string jsonString(const Json::Value &body,
                       const std::string &key,
                       const std::string &fallback = "");
Json::Value &arrayForLeague(std::unordered_map<std::string, Json::Value> &store,
                            const std::string &leagueId);

constexpr std::size_t kMaxLeaguesPerAccount = 3;"""
league = replace_once(league, anchor, forward_declarations, "league helper declarations")
league = replace_once(
    league,
    'std::string jsonString(const Json::Value &body, const std::string &key, const std::string &fallback = "") {',
    'std::string jsonString(const Json::Value &body, const std::string &key, const std::string &fallback) {',
    "jsonString default argument definition",
)
league_path.write_text(league, encoding="utf-8")

json_utils_path = Path("backend/src/json_utils.h")
json_utils = json_utils_path.read_text(encoding="utf-8")
json_utils = replace_once(
    json_utils,
    'std::string getStringOrDefault(const Json::Value &json, std::string_view key, std::string_view fallback);',
    'std::string getStringOrDefault(const Json::Value &json, std::string_view key, std::string_view fallback = "");',
    "getStringOrDefault default argument",
)
json_utils_path.write_text(json_utils, encoding="utf-8")

print("Applied backend source repairs successfully.")
