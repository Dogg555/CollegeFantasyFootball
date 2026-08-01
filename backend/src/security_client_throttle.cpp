#include <drogon/drogon.h>
#include <json/json.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>

namespace {

using Clock = std::chrono::steady_clock;

struct ClientBucket {
    std::deque<Clock::time_point> attempts;
    Clock::time_point lastSeen{Clock::now()};
};

std::mutex clientRateMutex;
std::unordered_map<std::string, ClientBucket> clientRateBuckets;

std::optional<std::string> clientEnv(const char *name) {
    const char *value = std::getenv(name);
    return value ? std::optional<std::string>{value} : std::nullopt;
}

bool clientEnvFlag(const char *name) {
    auto value = clientEnv(name).value_or("");
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

std::string clientTrim(std::string value) {
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), value.end());
    return value;
}

bool validClientAddress(std::string_view value) {
    if (value.empty() || value.size() > 64) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isdigit(ch) || std::isxdigit(ch) || ch == '.' || ch == ':' || ch == '%';
    });
}

std::string requestClient(const drogon::HttpRequestPtr &req) {
    if (clientEnvFlag("CFF_TRUST_PROXY_HEADERS")) {
        auto header = clientEnv("CFF_TRUSTED_CLIENT_IP_HEADER").value_or("x-forwarded-for");
        std::transform(header.begin(), header.end(), header.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        auto value = req->getHeader(header);
        if (header == "x-forwarded-for") {
            const auto comma = value.rfind(',');
            if (comma != std::string::npos) value = value.substr(comma + 1);
        } else {
            const auto comma = value.find(',');
            if (comma != std::string::npos) value.resize(comma);
        }
        value = clientTrim(std::move(value));
        if (validClientAddress(value)) return value;
    }
    const auto peer = req->getPeerAddr().toIp();
    return validClientAddress(peer) ? peer : "unknown";
}

std::string clientFingerprint(std::string_view value) {
    std::ostringstream output;
    output << std::hex << std::hash<std::string_view>{}(value);
    return output.str();
}

struct ClientPolicy {
    std::size_t limit;
    std::chrono::seconds window;
};

std::optional<ClientPolicy> clientPolicy(const drogon::HttpRequestPtr &req) {
    const auto &path = req->getPath();
    if (path == "/api/auth/login") return ClientPolicy{10, std::chrono::minutes(10)};
    if (path == "/api/auth/signup") return ClientPolicy{5, std::chrono::minutes(30)};
    if (path == "/api/auth/request-password-reset") return ClientPolicy{5, std::chrono::minutes(30)};
    if (path == "/api/auth/reset-password") return ClientPolicy{8, std::chrono::minutes(30)};
    if (path == "/api/auth/resend-verification") return ClientPolicy{5, std::chrono::minutes(30)};
    if (path == "/api/auth/verify-email") return ClientPolicy{20, std::chrono::minutes(15)};
    if (path.rfind("/api/admin/", 0) == 0) {
        return req->getMethod() == drogon::Get
            ? ClientPolicy{30, std::chrono::minutes(5)}
            : ClientPolicy{3, std::chrono::minutes(5)};
    }
    return std::nullopt;
}

bool clientRateAllowed(const std::string &key,
                       std::size_t limit,
                       std::chrono::seconds window) {
    const auto now = Clock::now();
    const auto cutoff = now - window;
    std::lock_guard<std::mutex> lock(clientRateMutex);
    auto &bucket = clientRateBuckets[key];
    bucket.lastSeen = now;
    while (!bucket.attempts.empty() && bucket.attempts.front() <= cutoff) {
        bucket.attempts.pop_front();
    }
    if (bucket.attempts.size() >= limit) return false;
    bucket.attempts.push_back(now);
    if (clientRateBuckets.size() > 25000) {
        for (auto it = clientRateBuckets.begin(); it != clientRateBuckets.end();) {
            if (it->second.lastSeen <= now - std::chrono::hours(1)) {
                it = clientRateBuckets.erase(it);
            } else {
                ++it;
            }
        }
    }
    return true;
}

drogon::HttpResponsePtr enforceClientRate(const drogon::HttpRequestPtr &req) {
    const auto policy = clientPolicy(req);
    if (!policy) return nullptr;
    const auto key = req->getPath() + ":client:" + clientFingerprint(requestClient(req));
    if (clientRateAllowed(key, policy->limit, policy->window)) return nullptr;

    Json::Value payload;
    payload["error"] = "Too many requests. Try again later.";
    payload["code"] = "rate_limited";
    auto response = drogon::HttpResponse::newHttpJsonResponse(payload);
    response->setStatusCode(static_cast<drogon::HttpStatusCode>(429));
    response->addHeader("Retry-After", std::to_string(policy->window.count()));
    response->addHeader("Cache-Control", "no-store");
    return response;
}

struct ClientRateInstaller {
    ClientRateInstaller() {
        drogon::app().registerSyncAdvice(enforceClientRate);
    }
};

ClientRateInstaller clientRateInstaller;

}  // namespace
