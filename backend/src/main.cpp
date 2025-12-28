#include <algorithm>
#include <array>
#include <cstdlib>
#include <chrono>
#include <iostream>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#ifdef DROGON_FOUND
#include <crypt.h>
#include <drogon/drogon.h>
#include <json/json.h>
#include "league_models.h"
#include "handlers/league_handler.h"
#include "player_catalog.h"
#endif

#ifdef DROGON_FOUND
namespace {
std::optional<std::string> readEnv(const std::string &key) {
    const char *val = std::getenv(key.c_str());
    if (val == nullptr) {
        return std::nullopt;
    }
    return std::string{val};
}

std::mutex userMutex;
std::unordered_map<std::string, std::string> userPasswordHashes;
std::unordered_map<std::string, std::string> activeTokens; // token -> email

std::optional<std::string> hashPassword(const std::string &password) {
    constexpr int kCost = 12;
    constexpr std::size_t kSaltLen = 16;
    std::array<unsigned char, kSaltLen> saltBytes{};
    std::random_device rd;
    for (auto &byte : saltBytes) {
        byte = static_cast<unsigned char>(rd());
    }
    char saltBuf[128];
    if (!crypt_gensalt_rn("$2b$", kCost,
                          reinterpret_cast<const char *>(saltBytes.data()),
                          saltBytes.size(),
                          saltBuf, sizeof(saltBuf))) {
        return std::nullopt;
    }
    struct crypt_data data;
    data.initialized = 0;
    const char *hash = crypt_r(password.c_str(), saltBuf, &data);
    if (!hash) {
        return std::nullopt;
    }
    return std::string{hash};
}

bool verifyPassword(const std::string &password, const std::string &hash) {
    struct crypt_data data;
    data.initialized = 0;
    const char *computed = crypt_r(password.c_str(), hash.c_str(), &data);
    if (!computed) {
        return false;
    }
    return hash == computed;
}

bool hasBearerToken(const drogon::HttpRequestPtr &req, std::string &outToken) {
    const auto authHeader = req->getHeader("authorization");
    if (authHeader.size() < 8) {
        return false;
    }
    constexpr std::string_view bearerPrefix = "Bearer ";
    if (authHeader.rfind(bearerPrefix, 0) != 0) {
        return false;
    }
    outToken = authHeader.substr(bearerPrefix.size());
    return true;
}

std::string randomToken() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::random_device rd;
    std::mt19937_64 gen(rd() ^ static_cast<std::mt19937_64::result_type>(now));
    std::uniform_int_distribution<std::uint64_t> dist;
    return "token-" + std::to_string(dist(gen));
}

std::string issueTokenForUser(const std::string &email) {
    const auto token = randomToken();
    std::lock_guard<std::mutex> lock(userMutex);
    activeTokens[token] = email;
    return token;
}

bool isAuthorized(const drogon::HttpRequestPtr &req, const std::optional<std::string> &secret) {
    std::string token;
    if (!hasBearerToken(req, token)) {
        return false;
    }
    if (secret && token == secret.value()) {
        return true; // compatibility for pre-shared secret
    }
    std::lock_guard<std::mutex> lock(userMutex);
    return activeTokens.find(token) != activeTokens.end();
}

std::optional<std::string> emailForToken(const std::string &token) {
    std::lock_guard<std::mutex> lock(userMutex);
    auto it = activeTokens.find(token);
    if (it == activeTokens.end()) {
        return std::nullopt;
    }
    return it->second;
}

void applyCorsHeaders(const drogon::HttpRequestPtr &req,
                      const drogon::HttpResponsePtr &resp,
                      const std::unordered_set<std::string> &allowedOrigins) {
    if (!allowedOrigins.empty()) {
        auto origin = req->getHeader("Origin");
        if (allowedOrigins.find(origin) != allowedOrigins.end()) {
            resp->addHeader("Access-Control-Allow-Origin", origin);
        }
    } else {
        resp->addHeader("Access-Control-Allow-Origin", "*");
    }
    resp->addHeader("Access-Control-Allow-Headers", "Authorization, Content-Type");
    resp->addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
}

drogon::HttpResponsePtr buildPreflightResponse(const drogon::HttpRequestPtr &req,
                                               const std::unordered_set<std::string> &allowedOrigins) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    applyCorsHeaders(req, resp, allowedOrigins);
    resp->setStatusCode(drogon::k204NoContent);
    return resp;
}

bool ensureCredentials(const Json::Value &body) {
    return body.isMember("email") && body["email"].isString()
        && body.isMember("password") && body["password"].isString()
        && !body["email"].asString().empty()
        && !body["password"].asString().empty();
}

void handleSignup(const drogon::HttpRequestPtr &req,
                  std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                  const std::optional<std::string> &jwtSecret) {
    (void)jwtSecret; // unused until persistent auth is wired
    const auto body = req->getJsonObject();
    if (!body || !ensureCredentials(*body)) {
        Json::Value error;
        error["error"] = "Email and password are required";
        auto resp = drogon::HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(drogon::k400BadRequest);
        callback(resp);
        return;
    }

    const auto email = (*body)["email"].asString();
    const auto password = (*body)["password"].asString();
    const auto passwordHash = hashPassword(password);
    if (!passwordHash) {
        Json::Value error;
        error["error"] = "Unable to create account";
        auto resp = drogon::HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(drogon::k500InternalServerError);
        callback(resp);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(userMutex);
        if (userPasswordHashes.find(email) != userPasswordHashes.end()) {
            Json::Value error;
            error["error"] = "Account already exists";
            auto resp = drogon::HttpResponse::newHttpJsonResponse(error);
            resp->setStatusCode(drogon::k409Conflict);
            callback(resp);
            return;
        }
        userPasswordHashes[email] = *passwordHash;
    }

    const auto token = issueTokenForUser(email);
    Json::Value payload;
    payload["email"] = email;
    payload["token"] = token;
    payload["valid"] = true;
    payload["message"] = "Account created";
    auto resp = drogon::HttpResponse::newHttpJsonResponse(payload);
    resp->setStatusCode(drogon::k201Created);
    callback(resp);
}

void handleLogin(const drogon::HttpRequestPtr &req,
                 std::function<void (const drogon::HttpResponsePtr &)> &&callback,
                 const std::optional<std::string> &jwtSecret) {
    (void)jwtSecret; // unused until persistent auth is wired
    const auto body = req->getJsonObject();
    if (!body || !ensureCredentials(*body)) {
        Json::Value error;
        error["error"] = "Email and password are required";
        auto resp = drogon::HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(drogon::k400BadRequest);
        callback(resp);
        return;
    }

    const auto email = (*body)["email"].asString();
    const auto password = (*body)["password"].asString();
    bool passwordMatches = false;
    {
        std::lock_guard<std::mutex> lock(userMutex);
        auto it = userPasswordHashes.find(email);
        passwordMatches = (it != userPasswordHashes.end() && verifyPassword(password, it->second));
    }

    if (!passwordMatches) {
        Json::Value error;
        error["error"] = "Invalid credentials";
        auto resp = drogon::HttpResponse::newHttpJsonResponse(error);
        resp->setStatusCode(drogon::k401Unauthorized);
        callback(resp);
        return;
    }

    const auto token = issueTokenForUser(email);
    Json::Value payload;
    payload["email"] = email;
    payload["token"] = token;
    payload["valid"] = true;
    payload["message"] = "Signed in";
    auto resp = drogon::HttpResponse::newHttpJsonResponse(payload);
    resp->setStatusCode(drogon::k200OK);
    callback(resp);
}

std::optional<std::string> getOptionalParam(const drogon::HttpRequestPtr &req, const std::string &key) {
    auto value = req->getParameter(key);
    if (value.empty()) {
        return std::nullopt;
    }
    return value;
}
} // namespace
#endif

int main(int argc, char* argv[]) {
#ifdef DROGON_FOUND
    auto &app = drogon::app();

    // Environment configuration
    const auto port = readEnv("PORT").value_or("8080");
    const auto jwtSecret = readEnv("JWT_SECRET");
    const auto sslCert = readEnv("SSL_CERT_FILE");
    const auto sslKey = readEnv("SSL_KEY_FILE");
    const auto allowedOriginEnv = readEnv("ALLOWED_ORIGINS");

    if (!jwtSecret.has_value()) {
        std::cerr << "[security] JWT_SECRET is not set; secure endpoints will reject all requests." << std::endl;
    }

    // SSL enablement when certs are available
    const bool useSsl = static_cast<bool>(sslCert && sslKey);
    if (sslCert && sslKey) {
        app.setSSLFiles(sslCert.value(), sslKey.value());
        std::cout << "[security] SSL enabled with provided certificate and key." << std::endl;
    } else {
        std::cout << "[security] SSL not configured. For testing only. Provide SSL_CERT_FILE and SSL_KEY_FILE to enable HTTPS." << std::endl;
    }

    // Minimal CORS handling via post-routing advice
    std::unordered_set<std::string> allowedOrigins;
    if (allowedOriginEnv) {
        // Comma-separated list of origins
        std::string list = allowedOriginEnv.value();
        std::size_t start = 0;
        while (true) {
            auto pos = list.find(',', start);
            auto origin = list.substr(start, pos == std::string::npos ? std::string::npos : pos - start);
            if (!origin.empty()) {
                allowedOrigins.insert(origin);
            }
            if (pos == std::string::npos) {
                break;
            }
            start = pos + 1;
        }
    }

    app.registerPostHandlingAdvice([allowedOrigins](const drogon::HttpRequestPtr &req,
                                                    const drogon::HttpResponsePtr &resp) {
        applyCorsHeaders(req, resp, allowedOrigins);
    });

    auto preflightHandler = [allowedOrigins](const drogon::HttpRequestPtr &req,
                                             std::function<void (const drogon::HttpResponsePtr &)> &&callback) {
        callback(buildPreflightResponse(req, allowedOrigins));
    };

    app.addListener("0.0.0.0", static_cast<unsigned short>(std::stoi(port)), useSsl)
        .setThreadNum(std::thread::hardware_concurrency())
        .registerHandler("/health", [](const drogon::HttpRequestPtr&, std::function<void (const drogon::HttpResponsePtr &)> &&callback) {
            auto resp = drogon::HttpResponse::newHttpResponse();
            resp->setStatusCode(drogon::k200OK);
            resp->setBody("ok");
            callback(resp);
        })
        .registerHandler("/api/secure/ping",
                         [jwtSecret](const drogon::HttpRequestPtr& req, std::function<void (const drogon::HttpResponsePtr &)> &&callback) {
                             auto resp = drogon::HttpResponse::newHttpResponse();
                             if (!isAuthorized(req, jwtSecret)) {
                                 resp->setStatusCode(drogon::k401Unauthorized);
                                 resp->setBody("unauthorized");
                                 callback(resp);
                                 return;
                             }
                             resp->setStatusCode(drogon::k200OK);
                             resp->setBody(R"({"status":"ok","scope":"secure"})");
                             resp->addHeader("Content-Type", "application/json");
                             callback(resp);
                        },
                         {drogon::Post, drogon::Get})
        .registerHandler("/api/auth/validate",
                         [jwtSecret](const drogon::HttpRequestPtr& req, std::function<void (const drogon::HttpResponsePtr &)> &&callback) {
                             auto resp = drogon::HttpResponse::newHttpResponse();
                             std::string token;
                             const bool hasToken = hasBearerToken(req, token);
                             const bool authorized = isAuthorized(req, jwtSecret);
                             Json::Value payload;
                             payload["valid"] = authorized;
                             if (authorized) {
                                 if (auto email = emailForToken(token)) {
                                     payload["email"] = *email;
                                 }
                             }
                             resp->setStatusCode(authorized ? drogon::k200OK : drogon::k401Unauthorized);
                             resp->setBody(payload.toStyledString());
                             resp->addHeader("Content-Type", "application/json");
                             callback(resp);
                         },
                         {drogon::Get})
        .registerHandler("/api/auth/login",
                         [jwtSecret](const drogon::HttpRequestPtr& req, std::function<void (const drogon::HttpResponsePtr &)> &&callback) {
                             handleLogin(req, std::move(callback), jwtSecret);
                         },
                         {drogon::Post})
        .registerHandler("/api/auth/signup",
                         [jwtSecret](const drogon::HttpRequestPtr& req, std::function<void (const drogon::HttpResponsePtr &)> &&callback) {
                             handleSignup(req, std::move(callback), jwtSecret);
                         },
                         {drogon::Post})
        .registerHandler("/api/leagues",
                         [jwtSecret](const drogon::HttpRequestPtr& req, std::function<void (const drogon::HttpResponsePtr &)> &&callback) {
                             if (!isAuthorized(req, jwtSecret)) {
                                 Json::Value error;
                                 error["error"] = "Unauthorized";
                                 auto resp = drogon::HttpResponse::newHttpJsonResponse(error);
                                 resp->setStatusCode(drogon::k401Unauthorized);
                                 callback(resp);
                                 return;
                             }
                             cff::handlers::handleCreateLeague(req, std::move(callback));
                         },
                         {drogon::Post})
        .registerHandler("/api/players",
                         [](const drogon::HttpRequestPtr& req, std::function<void (const drogon::HttpResponsePtr &)> &&callback) {
                             const auto query = req->getParameter("query");
                             if (query.empty()) {
                                 Json::Value error;
                                 error["error"] = "Query parameter is required";
                                 auto resp = drogon::HttpResponse::newHttpJsonResponse(error);
                                 resp->setStatusCode(drogon::k400BadRequest);
                                 callback(resp);
                                 return;
                             }

                             auto positionFilter = getOptionalParam(req, "position");
                             auto conferenceFilter = getOptionalParam(req, "conference");

                             std::size_t limit = 25;
                             const auto limitParam = req->getParameter("limit");
                             if (!limitParam.empty()) {
                                 char *end = nullptr;
                                 const auto parsed = std::strtoul(limitParam.c_str(), &end, 10);
                                 if (end != limitParam.c_str() && parsed > 0) {
                                     limit = std::min<std::size_t>(parsed, 50);
                                 }
                             }

                             const auto results = cff::searchPlayers(query, positionFilter, conferenceFilter, limit);
                             Json::Value payload(Json::arrayValue);
                             for (const auto &player : results) {
                                 payload.append(player.toJson());
                             }

                             auto resp = drogon::HttpResponse::newHttpJsonResponse(payload);
                             resp->setStatusCode(drogon::k200OK);
                             callback(resp);
                         },
                         {drogon::Get})
        .registerHandler("/api/secure/ping", preflightHandler, {drogon::Options})
        .registerHandler("/api/auth/validate", preflightHandler, {drogon::Options})
        .registerHandler("/api/auth/login", preflightHandler, {drogon::Options})
        .registerHandler("/api/auth/signup", preflightHandler, {drogon::Options})
        .registerHandler("/api/leagues", preflightHandler, {drogon::Options})
        .registerHandler("/api/players", preflightHandler, {drogon::Options})
        .run();
#else
    // Stub output to avoid hard dependency on Drogon in early scaffolding.
    std::cout << "College Fantasy Football backend scaffold (Drogon not linked)." << std::endl;
    std::cout << "Build with -DDROGON_FOUND=ON and link Drogon to run the HTTP server." << std::endl;
#endif
    return 0;
}
