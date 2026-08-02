#!/usr/bin/env python3
from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one match, found {count}")
    return text.replace(old, new, 1)


handler_path = Path("backend/src/handlers/league_handler.cpp")
handler = handler_path.read_text(encoding="utf-8")
old_sync = '''void dbSyncInvitedMembers(PGconn *conn,
                          const std::string &leagueId,
                          const std::string &commissionerEmail,
                          const Json::Value &invitedEmails) {
    if (!invitedEmails.isArray()) return;
    for (const auto &email : invitedEmails) {
        if (email.isString() && !email.asString().empty() && email.asString() != commissionerEmail) {
            dbUpsertMember(conn, leagueId, email.asString(), "member", "invited", commissionerEmail);
        }
    }
}
'''
new_sync = '''void dbSyncInvitedMembers(PGconn *conn,
                          const std::string &leagueId,
                          const std::string &commissionerEmail,
                          const Json::Value &invitedEmails) {
    if (!invitedEmails.isArray()) return;
    const auto normalizedCommissionerEmail = canonicalEmail(commissionerEmail);
    for (const auto &email : invitedEmails) {
        if (!email.isString()) continue;
        const auto memberEmail = canonicalEmail(email.asString());
        if (memberEmail.empty() || memberEmail == normalizedCommissionerEmail) continue;
        // League settings retain approved members in invitedEmails. Never demote an
        // active membership while synchronizing that compatibility list.
        if (dbIsActiveMember(conn, leagueId, memberEmail)) continue;
        dbUpsertMember(conn, leagueId, memberEmail, "member", "invited", normalizedCommissionerEmail);
    }
}
'''
handler = replace_once(handler, old_sync, new_sync, "preserve active invitation sync")
handler_path.write_text(handler, encoding="utf-8")

test_path = Path("scripts/authorization_security_tests.py")
test = test_path.read_text(encoding="utf-8")
old_test = '''    status, _ = request(f"/api/leagues/{league_id}", token=outsider)
    require(status == 200, "approved member lacks access")
    denied(f"/api/leagues/{league_id}", outsider, "PUT", {"name":"stolen","teams":8,"scoring":"ppr","draftType":"snake"})
'''
new_test = '''    status, _ = request(f"/api/leagues/{league_id}", token=outsider)
    require(status == 200, "approved member lacks access")
    status, body = request(f"/api/leagues/{league_id}", "PUT", {
        "name": "Authorization League Updated",
        "teams": 8,
        "scoring": "ppr",
        "draftType": "snake",
        "invitedEmails": [outsider_email.upper()],
        "rosterRules": {"qb":0,"rb":0,"wr":0,"te":0,"flex":0,"bench":8},
    }, owner)
    require(status == 200, f"league settings update failed: {status} {body}")
    status, _ = request(f"/api/leagues/{league_id}", token=outsider)
    require(status == 200, "saving league settings demoted an active member")
    denied(f"/api/leagues/{league_id}", outsider, "PUT", {"name":"stolen","teams":8,"scoring":"ppr","draftType":"snake"})
'''
test = replace_once(test, old_test, new_test, "active member settings regression")
test_path.write_text(test, encoding="utf-8")
