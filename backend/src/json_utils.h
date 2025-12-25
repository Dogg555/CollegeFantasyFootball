#pragma once

#include <json/json.h>
#include <optional>
#include <string>
#include <string_view>

namespace cff {

// Safely pull a string field from JSON.
std::optional<std::string> getString(const Json::Value &json, std::string_view key);

// Safely pull an integer field from JSON.
std::optional<int> getInt(const Json::Value &json, std::string_view key);

// Convenience wrappers with defaults.
std::string getStringOrDefault(const Json::Value &json, std::string_view key, std::string_view fallback);
int getIntOrDefault(const Json::Value &json, std::string_view key, int fallback);

} // namespace cff
