#!/usr/bin/env python3
from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one match, found {count}")
    return text.replace(old, new, 1)


path = Path("backend/src/handlers/league_handler.cpp")
text = path.read_text(encoding="utf-8")

old_db = '''    const auto ownerEmail = canonicalEmail(cell(owner.get(), 0, 0));
    const auto normalizedMemberEmail = canonicalEmail(memberEmail);
    if (normalizedMemberEmail == ownerEmail) return std::nullopt;
    const auto safeRole = role == "commissioner" ? "commissioner" : "member";
    if (safeRole == "commissioner" && canonicalEmail(accountEmail) != ownerEmail) return std::nullopt;
    auto safeStatus = statusForDb(status);
    if (!(safeStatus == "active" || safeStatus == "invited" || safeStatus == "removed")) {
        safeStatus = "invited";
    }
    if (!dbUpsertMember(conn.get(), leagueId, memberEmail, safeRole, safeStatus, accountEmail, teamName)) return std::nullopt;
    if (updateTeamName) {
        auto nameUpdate = execParams(conn.get(),
                                     "UPDATE league_members SET team_name = $3, updated_at = NOW() WHERE league_id = $1 AND email = $2",
                                     {leagueId, memberEmail, teamName});
        if (!resultOk(nameUpdate.get(), PGRES_COMMAND_OK)) return std::nullopt;
    }
    if (safeStatus == "removed") {
        auto removedInvite = execParams(conn.get(),
                                        "UPDATE leagues SET invited_emails = array_remove(invited_emails, $3), updated_at = NOW() "
                                        "WHERE id = $2 AND EXISTS (SELECT 1 FROM league_members WHERE league_id = $2 AND email = $1 AND role = 'commissioner' AND status = 'active')",
                                        {accountEmail, leagueId, memberEmail});
        (void)removedInvite;
    }
'''
new_db = '''    const auto ownerEmail = canonicalEmail(cell(owner.get(), 0, 0));
    const auto normalizedAccountEmail = canonicalEmail(accountEmail);
    const auto normalizedMemberEmail = canonicalEmail(memberEmail);
    const auto safeRole = role == "commissioner" ? "commissioner" : "member";
    auto safeStatus = statusForDb(status);
    if (!(safeStatus == "active" || safeStatus == "invited" || safeStatus == "removed")) {
        safeStatus = "invited";
    }
    if (normalizedMemberEmail == ownerEmail) {
        if (!updateTeamName || safeRole != "commissioner" || safeStatus != "active") return std::nullopt;
        auto nameUpdate = execParams(conn.get(),
                                     "UPDATE league_members SET team_name = $3, updated_at = NOW() WHERE league_id = $1 AND email = $2",
                                     {leagueId, normalizedMemberEmail, teamName});
        if (!resultOk(nameUpdate.get(), PGRES_COMMAND_OK) || std::string{PQcmdTuples(nameUpdate.get())} != "1") {
            return std::nullopt;
        }
        return membersForLeague(conn.get(), leagueId);
    }
    if (safeRole == "commissioner" && normalizedAccountEmail != ownerEmail) return std::nullopt;
    if (!dbUpsertMember(conn.get(), leagueId, normalizedMemberEmail, safeRole, safeStatus, normalizedAccountEmail, teamName)) return std::nullopt;
    if (updateTeamName) {
        auto nameUpdate = execParams(conn.get(),
                                     "UPDATE league_members SET team_name = $3, updated_at = NOW() WHERE league_id = $1 AND email = $2",
                                     {leagueId, normalizedMemberEmail, teamName});
        if (!resultOk(nameUpdate.get(), PGRES_COMMAND_OK)) return std::nullopt;
    }
    if (safeStatus == "removed") {
        auto removedInvite = execParams(conn.get(),
                                        "UPDATE leagues SET invited_emails = array_remove(invited_emails, $3), updated_at = NOW() "
                                        "WHERE id = $2 AND EXISTS (SELECT 1 FROM league_members WHERE league_id = $2 AND email = $1 AND role = 'commissioner' AND status = 'active')",
                                        {normalizedAccountEmail, leagueId, normalizedMemberEmail});
        (void)removedInvite;
    }
'''
text = replace_once(text, old_db, new_db, "database owner team-name update")

old_memory = '''    if (ownsLeagueLocked(normalizedMemberEmail, leagueId)) {
        sendError(callback, drogon::k403Forbidden, "The league owner cannot be demoted or removed");
        return;
    }
    if (role == "commissioner" && !ownsLeagueLocked(accountEmail, leagueId)) {
        sendError(callback, drogon::k403Forbidden, "Only the league owner may grant commissioner access");
        return;
    }
    auto &members = arrayForLeague(membersByLeague, leagueId);
    for (Json::ArrayIndex i = 0; i < members.size(); ++i) {
        if (canonicalEmail(jsonString(members[i], "email")) == normalizedMemberEmail) {
            members[i]["role"] = role == "commissioner" ? "commissioner" : "member";
            members[i]["status"] = status;
            if (body && body->isMember("teamName")) {
                members[i]["teamName"] = teamName;
            }
            callback(jsonResponse(members, drogon::k200OK));
            return;
        }
    }
'''
new_memory = '''    const auto safeRole = role == "commissioner" ? "commissioner" : "member";
    const auto safeStatus = lowerString(status);
    const auto ownerTarget = ownsLeagueLocked(normalizedMemberEmail, leagueId);
    if (ownerTarget && (!updateTeamName || safeRole != "commissioner" || safeStatus != "active")) {
        sendError(callback, drogon::k403Forbidden, "The league owner cannot be demoted or removed");
        return;
    }
    if (safeRole == "commissioner" && !ownsLeagueLocked(accountEmail, leagueId)) {
        sendError(callback, drogon::k403Forbidden, "Only the league owner may grant commissioner access");
        return;
    }
    auto &members = arrayForLeague(membersByLeague, leagueId);
    for (Json::ArrayIndex i = 0; i < members.size(); ++i) {
        if (canonicalEmail(jsonString(members[i], "email")) == normalizedMemberEmail) {
            if (!ownerTarget) {
                members[i]["role"] = safeRole;
                members[i]["status"] = status;
            }
            if (updateTeamName) {
                members[i]["teamName"] = teamName;
            }
            callback(jsonResponse(members, drogon::k200OK));
            return;
        }
    }
'''
text = replace_once(text, old_memory, new_memory, "in-memory owner team-name update")
path.write_text(text, encoding="utf-8")

test_path = Path("scripts/authorization_security_tests.py")
test = test_path.read_text(encoding="utf-8")
old_test = '''    owner_path = urllib.parse.quote(owner_email, safe="")
    status, _ = request(f"/api/leagues/{league_id}/members/{owner_path}", "PUT", {"role":"member","status":"Removed"}, owner)
    require(status == 403, "league owner could be removed")
'''
new_test = '''    owner_path = urllib.parse.quote(owner_email, safe="")
    status, body = request(f"/api/leagues/{league_id}/members/{owner_path}", "PUT", {"role":"commissioner","status":"Active","teamName":"Owner Team"}, owner)
    require(status == 200, f"owner team-name update failed: {status} {body}")
    owner_member = next((entry for entry in body if entry.get("email") == owner_email), None)
    require(owner_member and owner_member.get("teamName") == "Owner Team", "owner team name was not saved")
    status, _ = request(f"/api/leagues/{league_id}/members/{owner_path}", "PUT", {"role":"member","status":"Removed"}, owner)
    require(status == 403, "league owner could be removed")
'''
test = replace_once(test, old_test, new_test, "owner team-name regression")
test_path.write_text(test, encoding="utf-8")
