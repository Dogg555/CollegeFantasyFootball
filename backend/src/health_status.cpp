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

bool originAllowed(const std::string &origin) {
    if (origin.empty()) return true;
    const auto &origins = allowedOrigins();
    return origins.find(origin) != origins.end();
}

void applyCorsHeaders(const drogon::HttpRequestPtr &request,
                      const drogon::HttpResponsePtr &response) {
    const auto origin = request->getHeader("Origin");
    if (!origin.empty() && originAllowed(origin)) {
        response->addHeader("Access-Control-Allow-Origin", origin);
        response->addHeader("Vary", "Origin");
    }

    response->addHeader(
        "Access-Control-Allow-Headers",
        "Authorization, Content-Type, X-Request-ID, Idempotency-Key"
    );
    response->addHeader("Access-Control-Allow-Methods", "GET, POST, PUT, PATCH, DELETE, OPTIONS");
    response->addHeader("Access-Control-Max-Age", "600");
}

// Drogon's route matcher can reject OPTIONS before pre-routing advice runs when
// no route explicitly lists OPTIONS. A sync advice runs before route matching,
// so browser preflight requests receive a deterministic response for every API
// endpoint without duplicating an OPTIONS handler for each route.
drogon::HttpResponsePtr handleCorsPreflight(const drogon::HttpRequestPtr &request) {
    if (request->method() != drogon::Options) {
        return nullptr;
    }

    const auto origin = request->getHeader("Origin");
    if (!originAllowed(origin)) {
        Json::Value payload;
        payload["error"] = "Origin is not allowed";
        payload["code"] = "origin_not_allowed";
        auto response = drogon::HttpResponse::newHttpJsonResponse(payload);
        response->setStatusCode(drogon::k403Forbidden);
        response->addHeader("Vary", "Origin");
        return response;
    }

    auto response = drogon::HttpResponse::newHttpResponse();
    applyCorsHeaders(request, response);
    response->setStatusCode(drogon::k204NoContent);
    return response;
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
        drogon::app().registerSyncAdvice(handleCorsPreflight);
        drogon::app().registerPreSendingAdvice(enforceTruthfulHealthStatus);
    }
};

// security_hardening.cpp also registers a sync advice. On the production Linux
// toolchain, set an explicit initialization priority so preflight handling is
// registered first and an OPTIONS request is never parsed as a login payload.
#if defined(__GNUC__)
RuntimeAdviceInstaller runtimeAdviceInstaller __attribute__((init_priority(200)));
#else
RuntimeAdviceInstaller runtimeAdviceInstaller;
#endif

}  // namespace