#include "email_delivery.h"

#ifdef DROGON_FOUND
#include <drogon/drogon.h>
#include <json/json.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

namespace {

std::optional<std::string> envValue(const char *key) {
    const auto *value = std::getenv(key);
    if (!value || !*value) return std::nullopt;
    return std::string{value};
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

std::string frontendBaseUrl() {
    auto base = envValue("CFF_FRONTEND_BASE_URL").value_or("");
    if (base.empty()) {
        const auto origins = envValue("ALLOWED_ORIGINS").value_or("");
        const auto comma = origins.find(',');
        base = trim(origins.substr(0, comma));
    }
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    return base;
}

std::string urlEncode(const std::string &value) {
    std::ostringstream encoded;
    encoded << std::uppercase << std::hex;
    for (const unsigned char ch : value) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            encoded << static_cast<char>(ch);
        } else {
            encoded << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(ch);
        }
    }
    return encoded.str();
}

std::string htmlEscape(const std::string &value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const auto ch : value) {
        switch (ch) {
            case '&': escaped += "&amp;"; break;
            case '<': escaped += "&lt;"; break;
            case '>': escaped += "&gt;"; break;
            case '"': escaped += "&quot;"; break;
            case 39: escaped += "&#39;"; break;
            default: escaped.push_back(ch); break;
        }
    }
    return escaped;
}

bool memberInvitePath(const std::string &path, std::string &leagueId) {
    constexpr const char *prefix = "/api/leagues/";
    constexpr const char *suffix = "/members";
    const std::string prefixValue{prefix};
    const std::string suffixValue{suffix};
    if (path.rfind(prefixValue, 0) != 0 || path.size() <= prefixValue.size() + suffixValue.size()) {
        return false;
    }
    if (path.compare(path.size() - suffixValue.size(), suffixValue.size(), suffixValue) != 0) {
        return false;
    }
    leagueId = path.substr(prefixValue.size(), path.size() - prefixValue.size() - suffixValue.size());
    return !leagueId.empty() && leagueId.find('/') == std::string::npos;
}

void setDeliveryHeader(const drogon::HttpResponsePtr &response, const std::string &status) {
    response->addHeader("X-CFF-Invite-Email", status);
    response->addHeader("Access-Control-Expose-Headers", "X-CFF-Invite-Email");
}

void deliverInvite(const drogon::HttpRequestPtr &request,
                   const drogon::HttpResponsePtr &response,
                   const std::string &leagueId) {
    const auto body = request->getJsonObject();
    if (!body || !body->isObject() || !body->isMember("email") || !(*body)["email"].isString()) {
        return;
    }

    const auto recipient = trim((*body)["email"].asString());
    const auto baseUrl = frontendBaseUrl();
    if (recipient.empty() || baseUrl.empty() || !cff::emailDeliveryConfigured()) {
        setDeliveryHeader(response, "not-configured");
        std::cerr << "[league-invite] email delivery is not configured; invite remains saved for "
                  << recipient << std::endl;
        return;
    }

    const auto inviteLink = baseUrl + "/league.html?invite=" + urlEncode(leagueId);
    const auto sent = cff::sendTransactionalEmail(
        recipient,
        "You're invited to College Fantasy Football",
        "You've been invited to join a private College Fantasy Football league.\n\n"
        "Open your invite: " + inviteLink + "\n\n"
        "Sign in with this email address to request access from the league commissioner.",
        "<p>You've been invited to join a private College Fantasy Football league.</p>"
        "<p><a href=\"" + htmlEscape(inviteLink) + "\">Open league invite</a></p>"
        "<p>Sign in with this email address to request access from the league commissioner.</p>");

    setDeliveryHeader(response, sent ? "sent" : "failed");
    std::clog << "[league-invite] delivery " << (sent ? "sent" : "failed")
              << " recipient=" << recipient << " league=" << leagueId << std::endl;
}

[[maybe_unused]] const bool inviteEmailAdviceRegistered = []() {
    drogon::app().registerPostHandlingAdvice(
        [](const drogon::HttpRequestPtr &request, const drogon::HttpResponsePtr &response) {
            if (!request || !response || request->getMethod() != drogon::Post) return;
            const auto status = static_cast<int>(response->getStatusCode());
            if (status < 200 || status >= 300) return;

            std::string leagueId;
            if (!memberInvitePath(request->getPath(), leagueId)) return;
            deliverInvite(request, response, leagueId);
        });
    return true;
}();

}  // namespace
#endif
