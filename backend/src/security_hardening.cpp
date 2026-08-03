#include <drogon/drogon.h>
#include <json/json.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct RateBucket {
    std::deque<Clock::time_point> attempts;
    Clock::time_point lastSeen{Clock::now()};
};

std::mutex rateMutex;
std::unordered_map<std::string, RateBucket> rateBuckets;

std::optional<std::string> envValue(const char *name) {
    const char *value = std::getenv(name);
    if (!value) return std::nullopt;
    return std::string{value};
}

bool envFlag(const char *name, bool fallback = false) {
    const auto value = envValue(name);
    if (!value) return fallback;
    std::string lowered = *value;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return lowered == "1" || lowered == "true" || lowered == "yes" || lowered == "on";
}

std::size_t envSize(const char *name, std::size_t fallback, std::size_t maximum) {
    const auto value = envValue(name);
    if (!value || value->empty()) return fallback;
    char *end = nullptr;
    const auto parsed = std::strtoull(value->c_str(), &end, 10);
    if (end == value->c_str() || *end != '\0' || parsed == 0 || parsed > maximum) {
        return fallback;
    }
    return static_cast<std::size_t>(parsed);
}

std::string trim(std::string value) {
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), value.end());
    return value;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string canonicalEmail(std::string value) {
    return lower(trim(std::move(value)));
}

bool endsWith(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() && value.substr(value.size() - suffix.size()) == suffix;
}

bool isEmailFieldName(const std::string &name) {
    const auto normalized = lower(name);
    return normalized == "email" || endsWith(normalized, "email") || endsWith(normalized, "emails");
}

void normalizeEmailFields(Json::Value &value) {
    if (value.isArray()) {
        for (auto &item : value) normalizeEmailFields(item);
        return;
    }
    if (!value.isObject()) return;

    for (const auto &name : value.getMemberNames()) {
        auto &item = value[name];
        if (isEmailFieldName(name)) {
            if (item.isString()) {
                item = canonicalEmail(item.asString());
                continue;
            }
            if (item.isArray()) {
                for (auto &entry : item) {
                    if (entry.isString()) entry = canonicalEmail(entry.asString());
                }
                continue;
            }
        }
        normalizeEmailFields(item);
    }
}

std::unordered_set<std::string> csvSet(const char *name) {
    std::unordered_set<std::string> values;
    const auto raw = envValue(name);
    if (!raw) return values;
    std::size_t start = 0;
    while (start <= raw->size()) {
        const auto comma = raw->find(',', start);
        auto item = trim(raw->substr(start, comma == std::string::npos
            ? std::string::npos
            : comma - start));
        while (item.size() > 1 && item.back() == '/') item.pop_back();
        if (!item.empty()) values.insert(item);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return values;
}

bool containsControlCharacters(std::string_view value) {
    return std::any_of(value.begin(), value.end(), [](unsigned char ch) {
        return ch < 0x20 || ch == 0x7f;
    });
}

bool plausibleClientAddress(std::string_view value) {
    if (value.empty() || value.size() > 64 || containsControlCharacters(value)) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isdigit(ch) || std::isxdigit(ch) || ch == '.' || ch == ':' || ch == '%';
    });
}

bool safeRequestId(std::string_view value) {
    return !value.empty() && value.size() <= 128 && !containsControlCharacters(value);
}

std::string firstForwardedValue(std::string value) {
    const auto comma = value.find(',');
    if (comma != std::string::npos) value.resize(comma);
    return trim(std::move(value));
}

std::string lastForwardedValue(std::string value) {
    const auto comma = value.rfind(',');
    if (comma != std::string::npos) value = value.substr(comma + 1);
    return trim(std::move(value));
}

std::string requestId(const drogon::HttpRequestPtr &req) {
    for (const auto *header : {"rndr-id", "x-request-id"}) {
        auto candidate = trim(req->getHeader(header));
        if (safeRequestId(candidate)) return candidate;
    }
    return "";
}

std::string clientAddress(const drogon::HttpRequestPtr &req) {
    if (envFlag("CFF_TRUST_PROXY_HEADERS")) {
        const auto configured = lower(envValue("CFF_TRUSTED_CLIENT_IP_HEADER").value_or("x-forwarded-for"));
        if (configured == "x-forwarded-for") {
            auto candidate = lastForwardedValue(req->getHeader(configured));
            if (plausibleClientAddress(candidate)) return candidate;
        } else if (configured == "x-real-ip" || configured == "cf-connecting-ip") {
            auto candidate = firstForwardedValue(req->getHeader(configured));
            if (plausibleClientAddress(candidate)) return candidate;
        }
    }
    const auto peer = req->getPeerAddr().toIp();
    return plausibleClientAddress(peer) ? peer : "unknown";
}

std::string fingerprint(std::string_view value) {
    const auto hashed = std::hash<std::string_view>{}(value);
    std::ostringstream out;
    out << std::hex << hashed;
    return out.str();
}

std::string safeRoute(const drogon::HttpRequestPtr &req) {
    const auto matched = req->getMatchedPathPattern();
    if (!matched.empty()) return std::string{matched};
    const auto &path = req->getPath();
    if (path.rfind("/api/auth/", 0) == 0) return path;
    if (path == "/api/admin/ingest/cfbd/live" ||
        path == "/api/admin/ingest/cfbd/live/status") return path;
    if (path.rfind("/api/admin/", 0) == 0) return "/api/admin/*";
    if (path.rfind("/api/leagues", 0) == 0) return "/api/leagues/*";
    if (path.rfind("/api/players", 0) == 0) return "/api/players";
    if (path.rfind("/api/scores", 0) == 0) return "/api/scores/*";
    if (path == "/health" || path == "/api/health") return path;
    return "/other";
}

drogon::HttpResponsePtr jsonError(drogon::HttpStatusCode status,
                                  const std::string &message,
                                  const std::string &code) {
    Json::Value payload;
    payload["error"] = message;
    payload["code"] = code;
    auto response = drogon::HttpResponse::newHttpJsonResponse(payload);
    response->setStatusCode(status);
    response->addHeader("Cache-Control", "no-store");
    return response;
}

bool isMutation(drogon::HttpMethod method) {
    return method == drogon::Post || method == drogon::Put ||
           method == drogon::Patch || method == drogon::Delete;
}

bool isJsonContentType(std::string value) {
    value = lower(trim(std::move(value)));
    const auto semicolon = value.find(';');
    if (semicolon != std::string::npos) value.resize(semicolon);
    return value == "application/json";
}

bool looksLikeEmail(std::string_view email) {
    if (email.empty() || email.size() > 254 || containsControlCharacters(email)) return false;
    const auto at = email.find('@');
    if (at == std::string_view::npos || at == 0 || at + 1 >= email.size()) return false;
    if (email.find('@', at + 1) != std::string_view::npos) return false;
    const auto domain = email.substr(at + 1);
    if (domain.front() == '.' || domain.back() == '.' || domain.find('.') == std::string_view::npos) return false;
    return std::all_of(email.begin(), email.end(), [](unsigned char ch) {
        return std::isalnum(ch) || ch == '@' || ch == '.' || ch == '_' ||
               ch == '-' || ch == '+' || ch == '\'';
    });
}

bool validateEmailFields(const Json::Value &value, std::string &message) {
    if (value.isArray()) {
        for (const auto &item : value) {
            if (!validateEmailFields(item, message)) return false;
        }
        return true;
    }
    if (!value.isObject()) return true;

    for (const auto &name : value.getMemberNames()) {
        const auto &item = value[name];
        if (isEmailFieldName(name)) {
            if (item.isString()) {
                const auto email = item.asString();
                if (!email.empty() && !looksLikeEmail(email)) {
                    message = "A valid email address is required.";
                    return false;
                }
                continue;
            }
            if (item.isArray()) {
                if (item.size() > 24) {
                    message = "No more than 24 email invitations may be submitted at once.";
                    return false;
                }
                std::unordered_set<std::string> unique;
                for (const auto &entry : item) {
                    if (!entry.isString() || !looksLikeEmail(entry.asString())) {
                        message = "Every invitation must contain a valid email address.";
                        return false;
                    }
                    unique.insert(entry.asString());
                }
                if (unique.size() != item.size()) {
                    message = "Duplicate email invitations are not allowed.";
                    return false;
                }
                continue;
            }
        }
        if (!validateEmailFields(item, message)) return false;
    }
    return true;
}

bool commonPassword(std::string password) {
    password = lower(std::move(password));
    static const std::unordered_set<std::string> blocked = {
        "password", "password1", "password123", "12345678", "123456789",
        "qwerty123", "letmein123", "football123", "collegefootball",
        "collegefantasy", "admin123456", "welcome123", "changeme123",
        "iloveyou123", "abc123456", "111111111111", "000000000000"
    };
    return blocked.find(password) != blocked.end();
}

bool strongPassword(const std::string &password,
                    const std::string &email,
                    std::size_t minimum) {
    if (password.size() < minimum || password.size() > 72 || containsControlCharacters(password)) {
        return false;
    }
    if (commonPassword(password)) return false;
    const auto at = email.find('@');
    if (at != std::string::npos && at >= 4) {
        const auto local = lower(email.substr(0, at));
        if (!local.empty() && lower(password).find(local) != std::string::npos) return false;
    }
    const bool allSame = std::all_of(password.begin(), password.end(), [&](char ch) {
        return ch == password.front();
    });
    return !allSame;
}

std::string authSubject(const drogon::HttpRequestPtr &req) {
    const auto body = req->getJsonObject();
    if (!body || !body->isObject() || !body->isMember("email") || !(*body)["email"].isString()) {
        return "";
    }
    return fingerprint(canonicalEmail((*body)["email"].asString()));
}

bool takeRateLimit(const std::string &key,
                   std::size_t limit,
                   std::chrono::seconds window) {
    const auto now = Clock::now();
    const auto cutoff = now - window;
    std::lock_guard<std::mutex> lock(rateMutex);
    auto &bucket = rateBuckets[key];
    bucket.lastSeen = now;
    while (!bucket.attempts.empty() && bucket.attempts.front() <= cutoff) {
        bucket.attempts.pop_front();
    }
    if (bucket.attempts.size() >= limit) return false;
    bucket.attempts.push_back(now);

    if (rateBuckets.size() > 25000) {
        for (auto it = rateBuckets.begin(); it != rateBuckets.end();) {
            if (it->second.attempts.empty() || it->second.lastSeen <= now - std::chrono::hours(1)) {
                it = rateBuckets.erase(it);
            } else {
                ++it;
            }
        }
    }
    return true;
}

struct RatePolicy {
    std::size_t clientLimit;
    std::size_t accountLimit;
    std::chrono::seconds window;
    bool accountAware;
};

std::optional<RatePolicy> ratePolicy(const drogon::HttpRequestPtr &req) {
    const auto &path = req->getPath();
    if (path == "/api/auth/login") return RatePolicy{30, 10, std::chrono::minutes(10), true};
    if (path == "/api/auth/signup") {
        return RatePolicy{
            envSize("CFF_SIGNUP_CLIENT_LIMIT", 120, 5000),
            envSize("CFF_SIGNUP_ACCOUNT_LIMIT", 5, 100),
            std::chrono::minutes(envSize("CFF_SIGNUP_RATE_WINDOW_MINUTES", 60, 1440)),
            true
        };
    }
    if (path == "/api/auth/request-password-reset") return RatePolicy{20, 5, std::chrono::minutes(30), true};
    if (path == "/api/auth/reset-password") return RatePolicy{12, 0, std::chrono::minutes(30), false};
    if (path == "/api/auth/resend-verification") return RatePolicy{20, 5, std::chrono::minutes(30), true};
    if (path == "/api/auth/verify-email") return RatePolicy{30, 0, std::chrono::minutes(15), false};
    if (path == "/api/admin/ingest/cfbd/live") {
        return RatePolicy{4, 0, std::chrono::minutes(5), false};
    }
    if (path.rfind("/api/admin/", 0) == 0) {
        return req->getMethod() == drogon::Get
            ? RatePolicy{30, 0, std::chrono::minutes(5), false}
            : RatePolicy{3, 0, std::chrono::minutes(5), false};
    }
    if (path == "/api/players") return RatePolicy{120, 0, std::chrono::minutes(1), false};
    if (endsWith(path, "/join")) return RatePolicy{10, 0, std::chrono::minutes(30), false};
    if (path.find("/members") != std::string::npos && isMutation(req->getMethod())) {
        return RatePolicy{30, 0, std::chrono::hours(1), false};
    }
    return std::nullopt;
}

std::vector<std::string> configurationProblems() {
    std::vector<std::string> problems;
    if (!envFlag("CFF_SECURITY_ENFORCE_PRODUCTION")) return problems;
    if (envFlag("CFF_ALLOW_SHARED_SECRET_AUTH")) problems.emplace_back("shared-secret authentication is enabled");
    if (envFlag("CFF_EXPOSE_AUTH_TOKENS")) problems.emplace_back("authentication tokens are exposed in responses");
    if (envFlag("CFF_LOG_AUTH_TOKENS")) problems.emplace_back("authentication tokens are written to logs");

    const auto origins = csvSet("ALLOWED_ORIGINS");
    if (origins.empty()) problems.emplace_back("ALLOWED_ORIGINS is empty");
    if (origins.find("*") != origins.end()) problems.emplace_back("ALLOWED_ORIGINS contains a wildcard");

    const auto jwt = envValue("JWT_SECRET");
    if (!jwt || jwt->size() < 32) problems.emplace_back("JWT_SECRET is missing or shorter than 32 characters");

    const auto adminToken = envValue("CFF_ADMIN_API_TOKEN");
    if (adminToken && !adminToken->empty() && adminToken->size() < 32) {
        problems.emplace_back("CFF_ADMIN_API_TOKEN is shorter than 32 characters");
    }
    if (adminToken && jwt && *adminToken == *jwt) {
        problems.emplace_back("admin and JWT secrets are identical");
    }
    return problems;
}

const std::vector<std::string> startupProblems = configurationProblems();

bool allowedOrigin(const std::string &origin) {
    if (origin.empty()) return true;
    if (origin.size() > 512 || containsControlCharacters(origin)) return false;
    const auto origins = csvSet("ALLOWED_ORIGINS");
    return origins.find(origin) != origins.end();
}

drogon::HttpResponsePtr securityGate(const drogon::HttpRequestPtr &req) {
    const auto &path = req->getPath();
    const bool apiPath = path == "/health" || path.rfind("/api/", 0) == 0;
    if (!apiPath) return nullptr;

    if (!startupProblems.empty()) {
        return jsonError(drogon::k503ServiceUnavailable,
                         "Service security configuration is incomplete.",
                         "security_configuration_invalid");
    }

    const auto origin = req->getHeader("origin");
    if (!allowedOrigin(origin)) {
        return jsonError(drogon::k403Forbidden, "Origin is not allowed.", "origin_not_allowed");
    }

    constexpr std::size_t kAbsoluteMaximum = 10 * 1024 * 1024;
    const auto generalLimit = envSize("CFF_MAX_REQUEST_BODY_BYTES", 256 * 1024, kAbsoluteMaximum);
    const auto authLimit = envSize("CFF_AUTH_REQUEST_BODY_BYTES", 8 * 1024, generalLimit);
    const auto maximum = path.rfind("/api/auth/", 0) == 0 ? authLimit : generalLimit;
    const auto actualLength = std::max(req->bodyLength(), req->getRealContentLength());
    if (actualLength > maximum) {
        return jsonError(static_cast<drogon::HttpStatusCode>(413),
                         "Request body is too large.",
                         "request_too_large");
    }

    if (req->getQuery().size() > 4096 || req->getPath().size() > 2048) {
        return jsonError(static_cast<drogon::HttpStatusCode>(414),
                         "Request URI is too large.",
                         "request_uri_too_large");
    }

    if (isMutation(req->getMethod()) && req->bodyLength() > 0 &&
        !isJsonContentType(req->getHeader("content-type"))) {
        return jsonError(static_cast<drogon::HttpStatusCode>(415),
                         "Content-Type must be application/json.",
                         "unsupported_content_type");
    }

    if (path.rfind("/api/auth/", 0) == 0 && isMutation(req->getMethod())) {
        const auto burstLimit = envSize("CFF_AUTH_BURST_CLIENT_LIMIT", 240, 5000);
        const auto burstWindow = std::chrono::seconds(
            envSize("CFF_AUTH_BURST_WINDOW_SECONDS", 60, 3600));
        const auto burstKey = "/api/auth/*:burst:" + fingerprint(clientAddress(req));
        if (!takeRateLimit(burstKey, burstLimit, burstWindow)) {
            auto response = jsonError(static_cast<drogon::HttpStatusCode>(429),
                                      "Too many authentication requests. Try again shortly.",
                                      "rate_limited");
            response->addHeader("Retry-After", std::to_string(burstWindow.count()));
            return response;
        }
    }

    if (req->bodyLength() > 0 && isJsonContentType(req->getHeader("content-type"))) {
        const auto body = req->getJsonObject();
        if (!body || !body->isObject()) {
            return jsonError(drogon::k400BadRequest, "A valid JSON object is required.", "invalid_json");
        }
        normalizeEmailFields(*body);
        std::string emailError;
        if (!validateEmailFields(*body, emailError)) {
            return jsonError(drogon::k400BadRequest, emailError, "invalid_email");
        }
    }

    const bool authPayload = path == "/api/auth/signup" ||
                             path == "/api/auth/login" ||
                             path == "/api/auth/request-password-reset" ||
                             path == "/api/auth/resend-verification";
    if (authPayload && req->bodyLength() > 0) {
        const auto body = req->getJsonObject();
        if (!body || !body->isObject()) {
            return jsonError(drogon::k400BadRequest, "A valid JSON object is required.", "invalid_json");
        }
        if (!body->isMember("email") || !(*body)["email"].isString() ||
            !looksLikeEmail(trim((*body)["email"].asString()))) {
            return jsonError(drogon::k400BadRequest, "A valid email address is required.", "invalid_email");
        }
    }

    if (path == "/api/auth/signup" || path == "/api/auth/login") {
        const auto body = req->getJsonObject();
        if (!body || !body->isObject() ||
            !body->isMember("password") || !(*body)["password"].isString() ||
            (*body)["password"].asString().empty()) {
            return jsonError(drogon::k400BadRequest, "A password is required.", "invalid_password");
        }
    }

    if (path == "/api/auth/signup" || path == "/api/auth/reset-password") {
        const auto body = req->getJsonObject();
        const auto minimum = envSize("CFF_MIN_PASSWORD_LENGTH", 12, 64);
        if (!body || !body->isObject() ||
            !body->isMember("password") || !(*body)["password"].isString()) {
            return jsonError(drogon::k400BadRequest, "A password is required.", "invalid_password");
        }
        const auto email = body->isMember("email") && (*body)["email"].isString()
            ? (*body)["email"].asString()
            : std::string{};
        if (!strongPassword((*body)["password"].asString(), email, minimum)) {
            return jsonError(drogon::k400BadRequest,
                             "Password must be between " + std::to_string(minimum) +
                                 " and 72 characters and must not be commonly used.",
                             "weak_password");
        }
    }

    // Count only structurally valid authentication attempts against the strict
    // route/account policies. Malformed input is covered by the higher burst
    // ceiling above and cannot exhaust a legitimate user's signup allowance.
    if (const auto policy = ratePolicy(req)) {
        const auto route = safeRoute(req);
        const auto client = fingerprint(clientAddress(req));
        if (!takeRateLimit(route + ":client:" + client, policy->clientLimit, policy->window)) {
            auto response = jsonError(static_cast<drogon::HttpStatusCode>(429),
                                      "Too many requests. Try again later.",
                                      "rate_limited");
            response->addHeader("Retry-After", std::to_string(policy->window.count()));
            return response;
        }
        if (policy->accountAware) {
            const auto subject = authSubject(req);
            if (!subject.empty() &&
                !takeRateLimit(route + ":account:" + subject, policy->accountLimit, policy->window)) {
                auto response = jsonError(static_cast<drogon::HttpStatusCode>(429),
                                          "Too many requests. Try again later.",
                                          "rate_limited");
                response->addHeader("Retry-After", std::to_string(policy->window.count()));
                return response;
            }
        }
    }

    return nullptr;
}

void secureResponse(const drogon::HttpRequestPtr &req,
                    const drogon::HttpResponsePtr &resp) {
    resp->removeHeader("server");
    resp->addHeader("X-Content-Type-Options", "nosniff");
    resp->addHeader("X-Frame-Options", "DENY");
    resp->addHeader("Content-Security-Policy", "default-src 'none'; frame-ancestors 'none'");
    resp->addHeader("Referrer-Policy", "no-referrer");
    resp->addHeader("Permissions-Policy", "camera=(), microphone=(), geolocation=(), payment=(), usb=()");
    resp->addHeader("X-Permitted-Cross-Domain-Policies", "none");

    const auto &path = req->getPath();
    if (path.rfind("/api/", 0) == 0 || path == "/health") {
        resp->addHeader("Cache-Control", "no-store");
        resp->addHeader("Access-Control-Expose-Headers",
                        "X-CFF-Request-Id, Retry-After, X-CFF-Invite-Email");
    }

    const auto currentRequestId = requestId(req);
    if (!currentRequestId.empty()) {
        resp->addHeader("X-CFF-Request-Id", currentRequestId);
    }

    const auto forwardedProto = lower(firstForwardedValue(req->getHeader("x-forwarded-proto")));
    if (req->isOnSecureConnection() || forwardedProto == "https") {
        resp->addHeader("Strict-Transport-Security", "max-age=31536000; includeSubDomains");
    }
    if (req->getMethod() == drogon::Options) {
        resp->addHeader("Access-Control-Max-Age", "600");
    }

    const auto originalStatus = static_cast<int>(resp->getStatusCode());
    const bool verificationSignupAccepted =
        path == "/api/auth/signup" &&
        envFlag("CFF_REQUIRE_EMAIL_VERIFICATION") &&
        ((originalStatus >= 200 && originalStatus < 300) || originalStatus == 409);
    if (verificationSignupAccepted) {
        Json::Value accepted;
        accepted["status"] = "accepted";
        accepted["valid"] = false;
        accepted["signupAccepted"] = true;
        accepted["emailVerificationRequired"] = true;
        accepted["message"] =
            "Request accepted. Check your email for a verification link, or use Resend verification if it does not arrive.";
        const auto &jsonObject = resp->getJsonObject();
        if (jsonObject) {
            *jsonObject = accepted;
        } else {
            Json::StreamWriterBuilder writer;
            writer["indentation"] = "";
            resp->setBody(Json::writeString(writer, accepted));
        }
        resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        resp->setStatusCode(static_cast<drogon::HttpStatusCode>(202));
    }

    if (path == "/health" || path == "/api/health") {
        Json::Value minimal;
        minimal["status"] = startupProblems.empty() ? "ok" : "degraded";
        minimal["service"] = "college-ff-api";
        minimal["database"] = "unknown";

        Json::CharReaderBuilder builder;
        Json::Value original;
        std::string errors;
        std::istringstream input(std::string{resp->body()});
        if (Json::parseFromStream(builder, input, &original, &errors) && original.isObject()) {
            if (original.isMember("status") && original["status"].isString()) {
                minimal["status"] = original["status"];
            }
            if (original.isMember("database") && original["database"].isString()) {
                minimal["database"] = original["database"];
            }
        }
        if (!startupProblems.empty()) minimal["status"] = "degraded";
        Json::StreamWriterBuilder writer;
        writer["indentation"] = "";
        resp->setBody(Json::writeString(writer, minimal));
        resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    }

    const auto status = static_cast<int>(resp->getStatusCode());
    if (status == 401 || status == 403 || status == 413 ||
        status == 415 || status == 429 || status >= 500 ||
        path.rfind("/api/admin/", 0) == 0) {
        std::cerr << "[security] route=" << safeRoute(req)
                  << " status=" << status
                  << " client=" << fingerprint(clientAddress(req));
        if (!currentRequestId.empty()) {
            std::cerr << " request_id=" << currentRequestId;
        }
        std::cerr << std::endl;
    }
}

struct SecurityInstaller {
    SecurityInstaller() {
        if (!startupProblems.empty()) {
            std::cerr << "[security] refusing production traffic because "
                      << startupProblems.size() << " security setting(s) are invalid." << std::endl;
            for (const auto &problem : startupProblems) {
                std::cerr << "[security] configuration problem: " << problem << std::endl;
            }
        }
        const auto bodyLimit = envSize("CFF_MAX_REQUEST_BODY_BYTES", 256 * 1024, 10 * 1024 * 1024);
        drogon::app()
            .setClientMaxBodySize(bodyLimit)
            .setClientMaxMemoryBodySize(std::min<std::size_t>(bodyLimit, 64 * 1024))
            .setJsonParserStackLimit(64)
            .enableServerHeader(false)
            .registerSyncAdvice(securityGate)
            .registerPreSendingAdvice(secureResponse);
    }
};

SecurityInstaller securityInstaller;

}  // namespace
