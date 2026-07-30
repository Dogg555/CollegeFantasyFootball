#include <json/json.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>

// Forward declarations / small adapters used elsewhere in the codebase.
namespace cff {
    // Implementation lives elsewhere in the project.
    std::string getStringOrDefault(const Json::Value &v, const std::string &key, const std::string &fallback = "");
}

static std::string lowerString(const std::string &in) {
    std::string out = in;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c){ return std::tolower(c); });
    return out;
}

// isoNow() is declared/defined elsewhere in the project.
static std::string isoNow();

namespace {

bool waiverDeadlinePassed(const Json::Value &rules) {
    const auto deadline = cff::getStringOrDefault(rules, "claimDeadline", "");
    if (deadline.empty()) return true;
    return deadline <= isoNow();
}

std::vector<std::string> collectManagerEmails(const Json::Value &managers) {
    std::vector<std::string> emails;
    for (const auto &manager : managers) {
        const auto email = cff::getStringOrDefault(manager, "email", "");
        if (!email.empty()) emails.push_back(email);
    }
    return emails;
}

bool isCommissionerForAccount(const Json::Value &members, const std::string &accountEmail) {
    for (const auto &member : members) {
        if (cff::getStringOrDefault(member, "email", "") == accountEmail
            && cff::getStringOrDefault(member, "role", "") == "commissioner"
            && lowerString(cff::getStringOrDefault(member, "status", "Active")) != "removed") {
            return true;
        }
    }
    return false;
}

bool isMemberActive(const Json::Value &members, const std::string &accountEmail) {
    for (const auto &member : members) {
        if (cff::getStringOrDefault(member, "email", "") == accountEmail
            && lowerString(cff::getStringOrDefault(member, "status", "Active")) != "removed") {
            return true;
        }
    }
    return false;
}

bool isMemberActiveStrict(const Json::Value &members, const std::string &accountEmail) {
    for (const auto &member : members) {
        if (cff::getStringOrDefault(member, "email", "") == accountEmail
            && lowerString(cff::getStringOrDefault(member, "status", "Active")) == "active") {
            return true;
        }
    }
    return false;
}

} // namespace
