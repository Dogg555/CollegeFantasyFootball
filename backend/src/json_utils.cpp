#include "json_utils.h"

namespace cff {

std::optional<std::string> getString(const Json::Value &json, std::string_view key) {
    const auto &node = json[std::string{key}];
    if (node.isString()) {
        return node.asString();
    }
    return std::nullopt;
}

std::optional<int> getInt(const Json::Value &json, std::string_view key) {
    const auto &node = json[std::string{key}];
    if (node.isInt()) {
        return node.asInt();
    }
    return std::nullopt;
}

std::string getStringOrDefault(const Json::Value &json, std::string_view key, std::string_view fallback) {
    auto val = getString(json, key);
    return val ? *val : std::string{fallback};
}

int getIntOrDefault(const Json::Value &json, std::string_view key, int fallback) {
    auto val = getInt(json, key);
    return val ? *val : fallback;
}

} // namespace cff
