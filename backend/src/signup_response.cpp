#include <drogon/drogon.h>
#include <json/json.h>
#include <postgresql/libpq-fe.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

namespace {

struct PgConnDeleter {
    void operator()(PGconn *connection) const {
        if (connection) PQfinish(connection);
    }
};

struct PgResultDeleter {
    void operator()(PGresult *result) const {
        if (result) PQclear(result);
    }
};

using PgConnPtr = std::unique_ptr<PGconn, PgConnDeleter>;
using PgResultPtr = std::unique_ptr<PGresult, PgResultDeleter>;

enum class AccountPresence {
    Exists,
    Missing,
    Unavailable,
};

bool envFlag(const char *name) {
    const char *raw = std::getenv(name);
    if (!raw) return false;
    std::string value{raw};
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

std::string canonicalEmail(std::string value) {
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), value.end());
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

AccountPresence accountPresence(const std::string &email) {
    const char *url = std::getenv("DB_URL");
    if (!url || !*url || email.empty()) return AccountPresence::Unavailable;

    auto connection = PgConnPtr{PQconnectdb(url)};
    if (!connection || PQstatus(connection.get()) != CONNECTION_OK) {
        std::cerr << "[auth] unable to verify duplicate signup state" << std::endl;
        return AccountPresence::Unavailable;
    }

    const char *values[] = {email.c_str()};
    auto result = PgResultPtr{PQexecParams(
        connection.get(),
        "SELECT 1 FROM users WHERE email = $1 LIMIT 1",
        1,
        nullptr,
        values,
        nullptr,
        nullptr,
        0)};
    if (!result || PQresultStatus(result.get()) != PGRES_TUPLES_OK) {
        const char *sqlState = result ? PQresultErrorField(result.get(), PG_DIAG_SQLSTATE) : nullptr;
        std::cerr << "[auth] duplicate-state query failed sqlstate="
                  << (sqlState ? sqlState : "unknown") << std::endl;
        return AccountPresence::Unavailable;
    }
    return PQntuples(result.get()) > 0
        ? AccountPresence::Exists
        : AccountPresence::Missing;
}

void replaceJson(const drogon::HttpResponsePtr &response,
                 const Json::Value &payload,
                 drogon::HttpStatusCode status) {
    // newHttpJsonResponse retains a Json::Value and serializes it immediately
    // before transmission. Updating only setBody() is therefore overwritten by
    // the retained value. Mutate that value directly so Drogon generates the
    // intended response and calculates the correct content length.
    const auto jsonObject = response->getJsonObject();
    if (jsonObject) {
        *jsonObject = payload;
    } else {
        Json::StreamWriterBuilder writer;
        writer["indentation"] = "";
        response->setBody(Json::writeString(writer, payload));
    }
    response->removeHeader("Content-Length");
    response->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    response->setStatusCode(status);
    response->addHeader("Cache-Control", "no-store");
}

void normalizeDuplicateSignup(const drogon::HttpRequestPtr &request,
                              const drogon::HttpResponsePtr &response) {
    if (request->getPath() != "/api/auth/signup" ||
        static_cast<int>(response->getStatusCode()) != 409) {
        return;
    }

    const auto body = request->getJsonObject();
    const auto email = body && body->isObject() && body->isMember("email") && (*body)["email"].isString()
        ? canonicalEmail((*body)["email"].asString())
        : std::string{};
    const auto presence = accountPresence(email);

    if (presence == AccountPresence::Exists) {
        Json::Value accepted;
        accepted["status"] = "accepted";
        accepted["valid"] = false;
        accepted["accountMayExist"] = true;
        accepted["emailVerificationRequired"] = envFlag("CFF_REQUIRE_EMAIL_VERIFICATION");
        accepted["message"] =
            "Request accepted. Check your email if verification is required, or sign in if you already registered.";
        replaceJson(response, accepted, static_cast<drogon::HttpStatusCode>(202));
        return;
    }

    Json::Value failure;
    failure["error"] = presence == AccountPresence::Unavailable
        ? "Authentication storage is temporarily unavailable."
        : "Unable to create account.";
    failure["code"] = presence == AccountPresence::Unavailable
        ? "authentication_storage_unavailable"
        : "account_creation_failed";
    replaceJson(response,
                failure,
                presence == AccountPresence::Unavailable
                    ? drogon::k503ServiceUnavailable
                    : drogon::k500InternalServerError);
}

struct SignupResponseInstaller {
    SignupResponseInstaller() {
        drogon::app().registerPostHandlingAdvice(normalizeDuplicateSignup);
    }
};

SignupResponseInstaller signupResponseInstaller;

}  // namespace
