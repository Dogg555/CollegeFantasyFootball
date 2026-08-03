#include <drogon/drogon.h>
#include <json/json.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <string>
#include <unordered_set>

namespace {

std::string trim(std::string value) {
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), value.end());
    return value;
}

const std::unordered_set<std::string> &allowedOrigins() {
    static const auto origins = [] {
        std::unordered_set<std::string> parsed;
        const char *raw = std::getenv("ALLOWED_ORIGINS");
        if (!raw) return parsed;

        const std::string value{raw};
        std::size_t start = 0;
        while (start <= value.size()) {
            const auto comma = value.find(',', start);
            auto origin = trim(value.substr(
                start,
                comma == std::string::npos ? std::string::npos : comma - start
            ));
            if (!origin.empty()) parsed.insert(std::move(origin));
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        return parsed;
    }();
    return origins;
}

void applyCorsHeaders(const drogon::HttpRequestPtr &request,
                      const drogon::HttpResponsePtr &response) {
    const auto origin = request->getHeader("Origin");
    const auto &origins = allowedOrigins();
    if (!origin.empty() && origins.find(origin) != origins.end()) {
        response->addHeader("Access-Control-Allow-Origin", origin);
        response->addHeader("Vary", "Origin");
    }

    response->addHeader("Access-Control-Allow-Headers", "Authorization, Content-Type");
    response->addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
    response->addHeader("Access-Control-Max-Age", "600");
}

void handleCorsPreflight(const drogon::HttpRequestPtr &request,
                         drogon::AdviceCallback &&callback,
                         drogon::AdviceChainCallback &&chainCallback) {
    if (request->method() != drogon::Options) {
        chainCallback();
        return;
    }

    auto response = drogon::HttpResponse::newHttpResponse();
    applyCorsHeaders(request, response);
    response->setStatusCode(drogon::k204NoContent);
    callback(response);
}

bool isHealthPath(const drogon::HttpRequestPtr &request) {
    const auto &path = request->getPath();
    return path == "/health" || path == "/api/health";
}

void enforceTruthfulHealthStatus(const drogon::HttpRequestPtr &request,
                                 const drogon::HttpResponsePtr &response) {
    if (!isHealthPath(request)) return;

    const auto currentStatus = static_cast<int>(response->getStatusCode());
    if (currentStatus < 200 || currentStatus >= 300) return;

    Json::CharReaderBuilder builder;
    Json::Value payload;
    std::string errors;
    std::istringstream input{std::string{response->body()}};
    const bool parsed = Json::parseFromStream(builder, input, &payload, &errors);
    const bool healthy = parsed && payload.isObject() &&
                         payload.isMember("status") &&
                         payload["status"].isString() &&
                         payload["status"].asString() == "ok";

    if (!healthy) {
        response->setStatusCode(drogon::k503ServiceUnavailable);
    }
}

struct RuntimeAdviceInstaller {
    RuntimeAdviceInstaller() {
        drogon::app().registerPreRoutingAdvice(handleCorsPreflight);
        drogon::app().registerPreSendingAdvice(enforceTruthfulHealthStatus);
    }
};

RuntimeAdviceInstaller runtimeAdviceInstaller;

}  // namespace
