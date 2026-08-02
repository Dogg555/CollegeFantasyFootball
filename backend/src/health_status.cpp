#include <drogon/drogon.h>
#include <json/json.h>

#include <sstream>
#include <string>

namespace {

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

struct HealthStatusInstaller {
    HealthStatusInstaller() {
        drogon::app().registerPreSendingAdvice(enforceTruthfulHealthStatus);
    }
};

HealthStatusInstaller healthStatusInstaller;

}  // namespace
