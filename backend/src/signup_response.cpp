#include <drogon/drogon.h>
#include <json/json.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

namespace {

bool envFlag(const char *name) {
    const char *raw = std::getenv(name);
    if (!raw) return false;
    std::string value{raw};
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

void normalizeDuplicateSignup(const drogon::HttpRequestPtr &request,
                              const drogon::HttpResponsePtr &response) {
    if (request->getPath() != "/api/auth/signup" ||
        static_cast<int>(response->getStatusCode()) != 409) {
        return;
    }

    Json::Value payload;
    payload["status"] = "accepted";
    payload["valid"] = false;
    payload["accountMayExist"] = true;
    payload["emailVerificationRequired"] = envFlag("CFF_REQUIRE_EMAIL_VERIFICATION");
    payload["message"] =
        "Request accepted. Check your email if verification is required, or sign in if you already registered.";

    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    response->removeHeader("Content-Length");
    response->setBody(Json::writeString(writer, payload));
    response->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    response->setStatusCode(static_cast<drogon::HttpStatusCode>(202));
    response->addHeader("Cache-Control", "no-store");
}

struct SignupResponseInstaller {
    SignupResponseInstaller() {
        // Post-handling advice runs before pre-sending advice. Replacing the
        // body here ensures Drogon recalculates serialization metadata rather
        // than sending a 202 status with the original 409 JSON body.
        drogon::app().registerPostHandlingAdvice(normalizeDuplicateSignup);
    }
};

SignupResponseInstaller signupResponseInstaller;

}  // namespace
