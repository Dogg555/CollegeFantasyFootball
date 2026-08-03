#include <drogon/drogon.h>
#include <json/json.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

namespace {

bool envFlag(const char *name, bool fallback = false) {
    const char *raw = std::getenv(name);
    if (!raw) return fallback;
    std::string value{raw};
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

void hideVerificationSignupState(const drogon::HttpRequestPtr &request,
                                 const drogon::HttpResponsePtr &response) {
    if (request->getPath() != "/api/auth/signup" ||
        !envFlag("CFF_REQUIRE_EMAIL_VERIFICATION")) {
        return;
    }

    const auto status = static_cast<int>(response->getStatusCode());
    const bool successful = status >= 200 && status < 300;
    if (!successful && status != 409) return;

    Json::Value accepted;
    accepted["status"] = "accepted";
    accepted["valid"] = false;
    accepted["signupAccepted"] = true;
    accepted["emailVerificationRequired"] = true;
    accepted["message"] =
        "Request accepted. Check your email for a verification link, or use Resend verification if it does not arrive.";

    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    response->setBody(Json::writeString(writer, accepted));
    response->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    response->setStatusCode(static_cast<drogon::HttpStatusCode>(202));
    response->addHeader("Cache-Control", "no-store");
}

struct SignupResponseHardeningInstaller {
    SignupResponseHardeningInstaller() {
        drogon::app().registerPostHandlingAdvice(hideVerificationSignupState);
    }
};

SignupResponseHardeningInstaller signupResponseHardeningInstaller;

}  // namespace
