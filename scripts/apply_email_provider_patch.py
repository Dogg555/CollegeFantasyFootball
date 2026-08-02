#!/usr/bin/env python3
from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected one match, found {count}")
    return text.replace(old, new, 1)


main_path = Path("backend/src/main.cpp")
main = main_path.read_text(encoding="utf-8")
main = replace_once(main, '#include "cfbd_ingest.h"\n', '#include "cfbd_ingest.h"\n#include "email_delivery.h"\n', "email include")
old = '''bool emailDeliveryConfigured() {
    const auto apiKey = readEnv("RESEND_API_KEY");
    const auto from = readEnv("CFF_EMAIL_FROM");
    const auto frontend = frontendBaseUrl();
    return apiKey && !apiKey->empty() && from && !from->empty() && frontend.has_value();
}

bool sendEmail(const std::string &to,
               const std::string &subject,
               const std::string &text,
               const std::string &html) {
    const auto apiKey = readEnv("RESEND_API_KEY");
    const auto from = readEnv("CFF_EMAIL_FROM");
    if (!apiKey || apiKey->empty() || !from || from->empty()) {
        std::cerr << "[email] RESEND_API_KEY and CFF_EMAIL_FROM are required to send email." << std::endl;
        return false;
    }

    Json::Value payload;
    payload["from"] = *from;
    payload["to"].append(to);
    payload["subject"] = subject;
    payload["text"] = text;
    payload["html"] = html;

    const auto response = cpr::Post(
        cpr::Url{"https://api.resend.com/emails"},
        cpr::Header{{"Authorization", "Bearer " + *apiKey}, {"Content-Type", "application/json"}},
        cpr::Body{jsonToString(payload)}
    );
    if (response.status_code < 200 || response.status_code >= 300) {
        std::cerr << "[email] send failed status=" << response.status_code << " body=" << response.text << std::endl;
        return false;
    }
    return true;
}
'''
new = '''bool emailDeliveryConfigured() {
    return frontendBaseUrl().has_value() && cff::emailDeliveryConfigured();
}

bool sendEmail(const std::string &to,
               const std::string &subject,
               const std::string &text,
               const std::string &html) {
    return cff::sendTransactionalEmail(to, subject, text, html);
}
'''
main = replace_once(main, old, new, "email implementation delegation")
main_path.write_text(main, encoding="utf-8")

cmake_path = Path("backend/CMakeLists.txt")
cmake = cmake_path.read_text(encoding="utf-8")
cmake = replace_once(cmake, '    src/main.cpp\n', '    src/main.cpp\n    src/email_delivery.cpp\n', "email source")
cmake = replace_once(cmake, 'find_package(PostgreSQL REQUIRED)\n', 'find_package(PostgreSQL REQUIRED)\nfind_package(CURL REQUIRED)\n', "curl package")
cmake = replace_once(cmake, '    PostgreSQL::PostgreSQL\n', '    PostgreSQL::PostgreSQL\n    CURL::libcurl\n', "curl link")
cmake_path.write_text(cmake, encoding="utf-8")
