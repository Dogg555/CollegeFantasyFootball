#include "stat_ingestion_lifecycle.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

namespace cff::stat_ingestion_lifecycle {
namespace {

std::uint64_t fnv1a(const std::string &value) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char ch : value) {
        hash ^= static_cast<std::uint64_t>(ch);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string hashHex(const std::string &value) {
    std::ostringstream output;
    output << std::hex << std::setw(16) << std::setfill('0') << fnv1a(value);
    return output.str();
}

std::string stringValue(const Json::Value &value,
                        const char *key,
                        const std::string &fallback = "") {
    if (!value.isObject() || !value.isMember(key) || value[key].isNull()) return fallback;
    const auto &item = value[key];
    if (item.isString()) return item.asString();
    if (item.isInt64()) return std::to_string(item.asInt64());
    if (item.isUInt64()) return std::to_string(item.asUInt64());
    if (item.isDouble()) {
        std::ostringstream output;
        output << std::setprecision(17) << item.asDouble();
        return output.str();
    }
    return fallback;
}

} // namespace

std::string canonicalToken(std::string value) {
    std::string normalized;
    normalized.reserve(value.size());
    for (const unsigned char ch : value) {
        if (std::isalnum(ch)) normalized.push_back(static_cast<char>(std::tolower(ch)));
        else if ((ch == '_' || ch == '-' || std::isspace(ch))
                 && !normalized.empty() && normalized.back() != '_') {
            normalized.push_back('_');
        }
    }
    while (!normalized.empty() && normalized.back() == '_') normalized.pop_back();
    return normalized;
}

std::string statRecordKey(const Json::Value &record) {
    return canonicalToken(stringValue(record, "playerId")) + "|" +
           stringValue(record, "season", "0") + "|" +
           stringValue(record, "week", "0") + "|" +
           canonicalToken(stringValue(record, "category")) + "|" +
           canonicalToken(stringValue(record, "statName")) + "|" +
           stringValue(record, "gameId", "0");
}

std::string statSourceHash(const Json::Value &record) {
    std::ostringstream canonical;
    canonical << statRecordKey(record) << '|'
              << stringValue(record, "statValue", "0") << '|'
              << canonicalToken(stringValue(record, "team")) << '|'
              << canonicalToken(stringValue(record, "conference"));
    return hashHex(canonical.str());
}

int retryDelaySeconds(int attempt,
                      int retryAfterSeconds,
                      int maximumSeconds) {
    const int cap = std::max(1, maximumSeconds);
    if (retryAfterSeconds > 0) return std::min(retryAfterSeconds, cap);
    const int exponent = std::clamp(attempt - 1, 0, 10);
    const int delay = 5 * (1 << exponent);
    return std::min(delay, cap);
}

bool retryableProviderFailure(int statusCode, bool networkFailure) {
    return networkFailure || statusCode == 408 || statusCode == 425
        || statusCode == 429 || statusCode >= 500;
}

bool sourceFresh(long long ageSeconds, int staleAfterSeconds) {
    return ageSeconds >= 0 && ageSeconds <= std::max(1, staleAfterSeconds);
}

std::string recalculationStatus(const std::string &scoringStatus) {
    const auto normalized = canonicalToken(scoringStatus);
    if (normalized == "scored") return "pending";
    if (normalized == "final") return "blocked_final";
    return "not_required";
}

std::string recalculationReason(const std::string &scoringStatus) {
    const auto normalized = canonicalToken(scoringStatus);
    if (normalized == "scored") return "source_stats_corrected";
    if (normalized == "final") return "final_week_immutable";
    return "week_not_scored";
}

} // namespace cff::stat_ingestion_lifecycle
