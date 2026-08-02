#include "email_delivery.h"

#include <curl/curl.h>
#include <json/json.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

namespace cff {
namespace {

std::optional<std::string> env(const char *key) {
    const char *value = std::getenv(key);
    if (!value || !*value) return std::nullopt;
    return std::string{value};
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool safeHeader(const std::string &value) {
    return value.find('\r') == std::string::npos && value.find('\n') == std::string::npos;
}

std::string jsonString(const Json::Value &value) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, value);
}

std::string bareAddress(const std::string &value) {
    const auto left = value.find('<');
    const auto right = value.find('>', left == std::string::npos ? 0 : left + 1);
    if (left != std::string::npos && right != std::string::npos && right > left + 1) {
        return value.substr(left + 1, right - left - 1);
    }
    return value;
}

std::string safeLogText(std::string value) {
    std::replace(value.begin(), value.end(), '\r', ' ');
    std::replace(value.begin(), value.end(), '\n', ' ');
    constexpr std::size_t kMaxLogLength = 1000;
    if (value.size() > kMaxLogLength) {
        value.resize(kMaxLogLength);
        value += "...";
    }
    return value;
}

struct UploadBuffer {
    std::string data;
    std::size_t offset{0};
};

std::size_t readUpload(char *buffer, std::size_t size, std::size_t count, void *userdata) {
    auto *upload = static_cast<UploadBuffer *>(userdata);
    const auto capacity = size * count;
    const auto remaining = upload->data.size() - upload->offset;
    const auto bytes = std::min(capacity, remaining);
    if (bytes > 0) {
        std::copy_n(upload->data.data() + upload->offset, bytes, buffer);
        upload->offset += bytes;
    }
    return bytes;
}

std::size_t appendResponse(char *buffer, std::size_t size, std::size_t count, void *userdata) {
    auto *response = static_cast<std::string *>(userdata);
    const auto bytes = size * count;
    response->append(buffer, bytes);
    return bytes;
}

bool sendResend(const std::string &to,
                const std::string &subject,
                const std::string &text,
                const std::string &html) {
    const auto apiKey = env("RESEND_API_KEY");
    const auto from = env("CFF_EMAIL_FROM");
    if (!apiKey || !from) return false;

    Json::Value payload;
    payload["from"] = *from;
    payload["to"].append(to);
    payload["subject"] = subject;
    payload["text"] = text;
    payload["html"] = html;

    CURL *curl = curl_easy_init();
    if (!curl) return false;
    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, ("Authorization: Bearer " + *apiKey).c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");
    const auto body = jsonString(payload);
    std::string responseBody;
    curl_easy_setopt(curl, CURLOPT_URL, "https://api.resend.com/emails");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, appendResponse);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    const auto result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (result != CURLE_OK || status < 200 || status >= 300) {
        std::cerr << "[email] resend delivery failed status=" << status
                  << " curl=" << curl_easy_strerror(result);
        if (!responseBody.empty()) {
            std::cerr << " response=" << safeLogText(responseBody);
        }
        std::cerr << std::endl;
        return false;
    }
    return true;
}

bool sendSmtp(const std::string &to,
              const std::string &subject,
              const std::string &text,
              const std::string &html) {
    const auto host = env("CFF_SMTP_HOST");
    const auto username = env("CFF_SMTP_USERNAME");
    const auto password = env("CFF_SMTP_PASSWORD");
    const auto from = env("CFF_EMAIL_FROM");
    if (!host || !username || !password || !from) return false;
    if (!safeHeader(to) || !safeHeader(subject) || !safeHeader(*from)) return false;

    const auto port = env("CFF_SMTP_PORT").value_or("587");
    const auto mode = lower(env("CFF_SMTP_SECURITY").value_or("starttls"));
    const auto scheme = mode == "tls" || mode == "smtps" ? "smtps://" : "smtp://";
    const auto url = scheme + *host + ":" + port;
    const std::string boundary = "cff-transactional-boundary";

    std::ostringstream message;
    message << "From: " << *from << "\r\n"
            << "To: " << to << "\r\n"
            << "Subject: " << subject << "\r\n"
            << "MIME-Version: 1.0\r\n"
            << "Content-Type: multipart/alternative; boundary=\"" << boundary << "\"\r\n"
            << "\r\n"
            << "--" << boundary << "\r\n"
            << "Content-Type: text/plain; charset=utf-8\r\n"
            << "Content-Transfer-Encoding: 8bit\r\n\r\n"
            << text << "\r\n"
            << "--" << boundary << "\r\n"
            << "Content-Type: text/html; charset=utf-8\r\n"
            << "Content-Transfer-Encoding: 8bit\r\n\r\n"
            << html << "\r\n"
            << "--" << boundary << "--\r\n";

    UploadBuffer upload{message.str(), 0};
    CURL *curl = curl_easy_init();
    if (!curl) return false;
    struct curl_slist *recipients = nullptr;
    recipients = curl_slist_append(recipients, to.c_str());
    const auto envelopeFrom = bareAddress(*from);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERNAME, username->c_str());
    curl_easy_setopt(curl, CURLOPT_PASSWORD, password->c_str());
    curl_easy_setopt(curl, CURLOPT_MAIL_FROM, envelopeFrom.c_str());
    curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recipients);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, readUpload);
    curl_easy_setopt(curl, CURLOPT_READDATA, &upload);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (mode == "starttls") {
        curl_easy_setopt(curl, CURLOPT_USE_SSL, static_cast<long>(CURLUSESSL_ALL));
    }
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    const auto result = curl_easy_perform(curl);
    curl_slist_free_all(recipients);
    curl_easy_cleanup(curl);
    if (result != CURLE_OK) {
        std::cerr << "[email] smtp delivery failed: " << curl_easy_strerror(result) << std::endl;
        return false;
    }
    return true;
}

}  // namespace

std::string emailDeliveryProvider() {
    return lower(env("CFF_EMAIL_PROVIDER").value_or("resend"));
}

bool emailDeliveryConfigured() {
    const auto from = env("CFF_EMAIL_FROM");
    if (!from) return false;
    const auto provider = emailDeliveryProvider();
    if (provider == "smtp") {
        return env("CFF_SMTP_HOST").has_value() &&
               env("CFF_SMTP_USERNAME").has_value() &&
               env("CFF_SMTP_PASSWORD").has_value();
    }
    return provider == "resend" && env("RESEND_API_KEY").has_value();
}

bool sendTransactionalEmail(const std::string &to,
                            const std::string &subject,
                            const std::string &text,
                            const std::string &html) {
    if (!safeHeader(to) || !safeHeader(subject)) {
        std::cerr << "[email] rejected unsafe recipient or subject header" << std::endl;
        return false;
    }
    const auto provider = emailDeliveryProvider();
    if (provider == "smtp") return sendSmtp(to, subject, text, html);
    if (provider == "resend") return sendResend(to, subject, text, html);
    std::cerr << "[email] unsupported CFF_EMAIL_PROVIDER=" << provider << std::endl;
    return false;
}

}  // namespace cff
