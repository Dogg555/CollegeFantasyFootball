bool waiverDeadlinePassed(const Json::Value &rules) {
    const auto deadline = cff::getStringOrDefault(rules, "claimDeadline", "");
    if (deadline.empty()) return true;
    return deadline <= isoNow();
}


for (const auto &manager : managers) {
    const auto email = cff::getStringOrDefault(manager, "email", "");
    if (!email.empty()) {
        emails.push_back(email);
    }
}


for (const auto &member : membersIt->second) {
    if (cff::getStringOrDefault(member, "email", "") == accountEmail
        && cff::getStringOrDefault(member, "role", "") == "commissioner"
        && lowerString(cff::getStringOrDefault(member, "status", "Active")) != "removed") {
        return true;
    }
}

for (const auto &member : membersIt->second) {
    if (cff::getStringOrDefault(member, "email", "") == accountEmail
        && lowerString(cff::getStringOrDefault(member, "status", "Active")) != "removed") {
        return true;
    }
}

for (const auto &member : membersIt->second) {
    if (cff::getStringOrDefault(member, "email", "") == accountEmail
        && lowerString(cff::getStringOrDefault(member, "status", "Active")) == "active") {
        return true;
    }
}

return false;
